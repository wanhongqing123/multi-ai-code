import { spawn } from 'node:child_process'
import { createHash, randomUUID } from 'node:crypto'
import { promises as fs } from 'node:fs'
import { tmpdir } from 'node:os'
import { basename, isAbsolute, join, relative, resolve, sep } from 'node:path'
import type {
  RemoteImGitDiffArtifact,
  RemoteImGitDiffArtifactSource
} from './types.js'

const COMMAND_TIMEOUT_MS = 20_000
const MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
const MAX_REPORT_BYTES = 4_500_000
const MAX_UNTRACKED_FILE_BYTES = 512 * 1024
const MAX_EXPANDED_UNTRACKED_FILES = 100
const MAX_SUMMARY_FILES = 40
const MAX_SUMMARY_CHARS = 6000
const MAX_STORED_REPORTS = 20
const REPORT_MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000
const MAX_HTML_DIFF_SOURCE_BYTES = 768 * 1024

interface GitCommandResult {
  code: number
  stdout: string
  stderr: string
  truncated: boolean
}

interface GitChange {
  status: string
  path: string
  oldPath?: string
  additions: number | null
  deletions: number | null
  sensitive: boolean
  contentOmitted?: string
}

interface RepositoryDiff {
  label: string
  root: string
  changes: GitChange[]
  diff: string
  truncated: boolean
}

interface ParsedDiffArgs {
  statOnly: boolean
  scope?: string
  source:
    | { kind: 'working' }
    | { kind: 'commit'; ref: string }
    | { kind: 'range'; base: string; head: string }
}

type ResolvedDiffSource = RemoteImGitDiffArtifactSource

export interface CreateGitDiffReportInput {
  targetRepo: string
  args?: string
  outputDir: string
  now?: () => number
}

export type CreateGitDiffReportResult =
  | {
      ok: true
      text: string
      attachmentPath?: string
      artifact?: RemoteImGitDiffArtifact
    }
  | {
      ok: false
      error: string
      text: string
    }

function runGit(
  cwd: string,
  args: string[],
  options: {
    allowedExitCodes?: number[]
    maxOutputBytes?: number
    timeoutMs?: number
  } = {}
): Promise<GitCommandResult> {
  const allowedExitCodes = options.allowedExitCodes ?? [0]
  const maxOutputBytes = options.maxOutputBytes ?? MAX_COMMAND_OUTPUT_BYTES
  const timeoutMs = options.timeoutMs ?? COMMAND_TIMEOUT_MS

  return new Promise((resolveCommand, rejectCommand) => {
    const child = spawn('git', ['-c', 'color.ui=false', '-c', 'core.quotepath=false', ...args], {
      cwd,
      shell: false,
      windowsHide: true,
      env: {
        ...process.env,
        GIT_OPTIONAL_LOCKS: '0'
      }
    })
    const stdout: Buffer[] = []
    const stderr: Buffer[] = []
    let stdoutBytes = 0
    let stderrBytes = 0
    let truncated = false
    let timedOut = false

    const timeout = setTimeout(() => {
      timedOut = true
      child.kill()
    }, timeoutMs)

    child.stdout.on('data', (chunk: Buffer) => {
      if (stdoutBytes >= maxOutputBytes) {
        truncated = true
        return
      }
      const remaining = maxOutputBytes - stdoutBytes
      const kept = chunk.byteLength > remaining ? chunk.subarray(0, remaining) : chunk
      stdout.push(kept)
      stdoutBytes += kept.byteLength
      if (kept.byteLength < chunk.byteLength) truncated = true
    })
    child.stderr.on('data', (chunk: Buffer) => {
      const remaining = 64 * 1024 - stderrBytes
      if (remaining <= 0) return
      const kept = chunk.byteLength > remaining ? chunk.subarray(0, remaining) : chunk
      stderr.push(kept)
      stderrBytes += kept.byteLength
    })
    child.once('error', (error) => {
      clearTimeout(timeout)
      rejectCommand(error)
    })
    child.once('close', (code) => {
      clearTimeout(timeout)
      const result: GitCommandResult = {
        code: code ?? -1,
        stdout: Buffer.concat(stdout).toString('utf8'),
        stderr: Buffer.concat(stderr).toString('utf8').trim(),
        truncated
      }
      if (timedOut) {
        rejectCommand(new Error(`Git command timed out: git ${args.join(' ')}`))
        return
      }
      if (!allowedExitCodes.includes(result.code)) {
        rejectCommand(
          new Error(result.stderr || `Git command failed (${result.code}): git ${args.join(' ')}`)
        )
        return
      }
      resolveCommand(result)
    })
  })
}

function parseDiffArgs(args?: string): ParsedDiffArgs | { error: string } {
  let remaining = args?.trim() ?? ''
  let statOnly = false
  let all = false
  let source: ParsedDiffArgs['source'] = { kind: 'working' }

  for (;;) {
    const match = /^(--stat|--all)(?:\s+|$)/.exec(remaining)
    if (!match) break
    if (match[1] === '--stat') statOnly = true
    if (match[1] === '--all') all = true
    remaining = remaining.slice(match[0].length).trim()
  }

  const commitMatch = /^--commit(?:\s+([^\s]+))(?:\s+|$)/.exec(remaining)
  const rangeMatch = /^--range(?:\s+([^\s]+)\.\.([^\s]+))(?:\s+|$)/.exec(remaining)
  const workingMatch = /^--working(?:\s+|$)/.exec(remaining)
  if (commitMatch?.[1]) {
    if (commitMatch[1].startsWith('-')) return { error: '提交引用不能以 - 开头。' }
    source = { kind: 'commit', ref: commitMatch[1] }
    remaining = remaining.slice(commitMatch[0].length).trim()
  } else if (rangeMatch?.[1] && rangeMatch[2]) {
    if (rangeMatch[1].startsWith('-') || rangeMatch[2].startsWith('-')) {
      return { error: '范围引用不能以 - 开头。' }
    }
    source = { kind: 'range', base: rangeMatch[1], head: rangeMatch[2] }
    remaining = remaining.slice(rangeMatch[0].length).trim()
  } else if (workingMatch) {
    remaining = remaining.slice(workingMatch[0].length).trim()
  }

  if (all && source.kind !== 'working') {
    return { error: '--all 只适用于工作区 Diff。' }
  }

  if (remaining.startsWith('--')) {
    return {
      error: '用法：/diff [--stat] [--working | --commit <ref> | --range <base>..<head>] [文件或目录]'
    }
  }
  if (all && remaining) {
    return { error: '/diff --all 不能再指定文件路径。' }
  }

  if (
    remaining.length >= 2 &&
    ((remaining.startsWith('"') && remaining.endsWith('"')) ||
      (remaining.startsWith("'") && remaining.endsWith("'")))
  ) {
    remaining = remaining.slice(1, -1)
  }

  return {
    statOnly,
    source,
    ...(remaining && !all ? { scope: remaining } : {})
  }
}

function splitNul(text: string): string[] {
  const tokens = text.split('\0')
  if (tokens.at(-1) === '') tokens.pop()
  return tokens
}

function normalizeGitPath(path: string): string {
  return path.split(sep).join('/')
}

function isInside(parent: string, child: string): boolean {
  const value = relative(parent, child)
  return value === '' || (!value.startsWith(`..${sep}`) && value !== '..' && !isAbsolute(value))
}

function parseNameStatus(text: string): GitChange[] {
  const tokens = splitNul(text)
  const changes: GitChange[] = []
  let index = 0
  while (index < tokens.length) {
    let statusToken = tokens[index++] ?? ''
    let embeddedPath = ''
    const tab = statusToken.indexOf('\t')
    if (tab >= 0) {
      embeddedPath = statusToken.slice(tab + 1)
      statusToken = statusToken.slice(0, tab)
    }
    if (!statusToken) continue

    const status = statusToken[0]
    const firstPath = embeddedPath || tokens[index++] || ''
    if (!firstPath) continue
    if (status === 'R' || status === 'C') {
      const nextPath = tokens[index++] || ''
      if (!nextPath) continue
      changes.push({
        status: statusToken,
        path: nextPath,
        oldPath: firstPath,
        additions: null,
        deletions: null,
        sensitive: isSensitivePath(firstPath) || isSensitivePath(nextPath)
      })
      continue
    }
    changes.push({
      status: statusToken,
      path: firstPath,
      additions: null,
      deletions: null,
      sensitive: isSensitivePath(firstPath)
    })
  }
  return changes
}

function parseNumStat(text: string): Map<string, { additions: number | null; deletions: number | null }> {
  const tokens = splitNul(text)
  const stats = new Map<string, { additions: number | null; deletions: number | null }>()
  let index = 0
  while (index < tokens.length) {
    const token = tokens[index++] ?? ''
    const firstTab = token.indexOf('\t')
    const secondTab = firstTab >= 0 ? token.indexOf('\t', firstTab + 1) : -1
    if (firstTab < 0 || secondTab < 0) continue
    const additionsText = token.slice(0, firstTab)
    const deletionsText = token.slice(firstTab + 1, secondTab)
    let path = token.slice(secondTab + 1)
    if (!path) {
      index += 1 // old path for a rename/copy
      path = tokens[index++] ?? ''
    }
    if (!path) continue
    stats.set(path, {
      additions: additionsText === '-' ? null : Number(additionsText),
      deletions: deletionsText === '-' ? null : Number(deletionsText)
    })
  }
  return stats
}

function isSensitivePath(path: string): boolean {
  const normalized = path.replaceAll('\\', '/').toLowerCase()
  const name = normalized.split('/').pop() ?? normalized
  if (name === '.env' || (name.startsWith('.env.') && !/\.(example|sample|template)$/.test(name))) {
    return true
  }
  if (['.npmrc', '.pypirc', 'id_rsa', 'id_ed25519', 'credentials.json', 'service-account.json'].includes(name)) {
    return true
  }
  return /\.(pem|key|p12|pfx)$/.test(name)
}

function pathspec(scope?: string, excludedPaths: string[] = []): string[] {
  return [
    '--',
    scope || '.',
    ...excludedPaths.map((path) => `:(exclude,literal)${path}`)
  ]
}

function quotePatchPath(path: string): string {
  const clean = path.replaceAll('\\', '/')
  return /^[A-Za-z0-9_./@+\-]+$/.test(clean) ? clean : JSON.stringify(clean)
}

function rewriteUntrackedPatch(patch: string, path: string): string {
  const quoted = quotePatchPath(path)
  return patch
    .split('\n')
    .map((line) => {
      if (line.startsWith('diff --git ')) return `diff --git a/${quoted} b/${quoted}`
      if (line.startsWith('--- ')) return '--- /dev/null'
      if (line.startsWith('+++ ')) return `+++ b/${quoted}`
      return line
    })
    .join('\n')
}

async function expandUntrackedFiles(input: {
  repoRoot: string
  paths: string[]
  emptyFile: string
}): Promise<{ changes: GitChange[]; patches: string[]; truncated: boolean }> {
  const changes: GitChange[] = []
  const patches: string[] = []
  let expanded = 0
  let truncated = false

  for (const path of input.paths) {
    const sensitive = isSensitivePath(path)
    const change: GitChange = {
      status: '??',
      path,
      additions: null,
      deletions: null,
      sensitive
    }
    changes.push(change)
    if (sensitive) {
      change.contentOmitted = '敏感文件内容已隐藏'
      continue
    }
    if (expanded >= MAX_EXPANDED_UNTRACKED_FILES) {
      change.contentOmitted = '未跟踪文件过多，内容已省略'
      truncated = true
      continue
    }

    const absolutePath = resolve(input.repoRoot, path)
    let stat
    try {
      stat = await fs.lstat(absolutePath)
    } catch {
      change.contentOmitted = '文件在生成报告前已消失'
      continue
    }
    if (!stat.isFile()) {
      change.contentOmitted = stat.isSymbolicLink() ? '符号链接内容未展开' : '非普通文件'
      continue
    }
    if (stat.size > MAX_UNTRACKED_FILE_BYTES) {
      change.contentOmitted = `文件大于 ${MAX_UNTRACKED_FILE_BYTES / 1024} KiB，内容已省略`
      continue
    }

    const sample = await fs.readFile(absolutePath)
    if (sample.includes(0)) {
      change.contentOmitted = '二进制文件内容未展开'
      continue
    }
    const text = sample.toString('utf8')
    change.additions = text ? text.split('\n').length - (text.endsWith('\n') ? 1 : 0) : 0
    change.deletions = 0
    const result = await runGit(
      input.repoRoot,
      ['diff', '--no-index', '--no-ext-diff', '--no-textconv', '--no-color', '--', input.emptyFile, absolutePath],
      { allowedExitCodes: [0, 1], maxOutputBytes: MAX_UNTRACKED_FILE_BYTES * 3 }
    )
    if (result.stdout.trim()) patches.push(rewriteUntrackedPatch(result.stdout, path))
    if (result.truncated) {
      change.contentOmitted = 'Diff 过长，已截断'
      truncated = true
    }
    expanded += 1
  }

  return { changes, patches, truncated }
}

function applyNumStats(changes: GitChange[], stats: Map<string, { additions: number | null; deletions: number | null }>): void {
  for (const change of changes) {
    const stat = stats.get(change.path)
    if (!stat) continue
    change.additions = stat.additions
    change.deletions = stat.deletions
  }
}

async function hasHead(repoRoot: string): Promise<boolean> {
  const result = await runGit(repoRoot, ['rev-parse', '--verify', '--quiet', 'HEAD'], {
    allowedExitCodes: [0, 1, 128],
    maxOutputBytes: 1024
  })
  return result.code === 0
}

async function listSubmodules(repoRoot: string): Promise<string[]> {
  const result = await runGit(repoRoot, ['ls-files', '--stage', '-z'], {
    maxOutputBytes: 2 * 1024 * 1024
  })
  if (result.truncated) return []
  const paths: string[] = []
  for (const record of splitNul(result.stdout)) {
    const match = /^(\d+)\s+[0-9a-f]+\s+\d+\t([\s\S]+)$/.exec(record)
    if (match?.[1] === '160000' && match[2]) paths.push(match[2])
  }
  return paths
}

function submoduleScope(scope: string | undefined, submodulePath: string): string | null | undefined {
  if (!scope) return undefined
  const cleanScope = scope.replace(/^\.\//, '').replace(/\/$/, '')
  const cleanSubmodule = submodulePath.replace(/\/$/, '')
  if (cleanScope === cleanSubmodule) return undefined
  if (cleanScope.startsWith(`${cleanSubmodule}/`)) {
    return cleanScope.slice(cleanSubmodule.length + 1)
  }
  if (cleanSubmodule.startsWith(`${cleanScope}/`)) return undefined
  return null
}

async function collectRepositoryDiff(input: {
  repoRoot: string
  label: string
  scope?: string
  emptyFile: string
}): Promise<RepositoryDiff[]> {
  const headExists = await hasHead(input.repoRoot)
  const commonDiffArgs = ['--no-ext-diff', '--no-textconv', '--find-renames', '--ignore-submodules=dirty']
  let trackedChanges: GitChange[] = []
  let trackedDiff = ''
  let trackedTruncated = false

  if (headExists) {
    const names = await runGit(input.repoRoot, [
      'diff',
      ...commonDiffArgs,
      '--name-status',
      '-z',
      'HEAD',
      ...pathspec(input.scope)
    ])
    trackedChanges = parseNameStatus(names.stdout)
    const stats = await runGit(input.repoRoot, [
      'diff',
      ...commonDiffArgs,
      '--numstat',
      '-z',
      'HEAD',
      ...pathspec(input.scope)
    ])
    applyNumStats(trackedChanges, parseNumStat(stats.stdout))

    const sensitivePaths = trackedChanges
      .filter((change) => change.sensitive)
      .flatMap((change) => [change.path, ...(change.oldPath ? [change.oldPath] : [])])
    const full = await runGit(input.repoRoot, [
      'diff',
      ...commonDiffArgs,
      '--no-color',
      'HEAD',
      ...pathspec(input.scope, sensitivePaths)
    ])
    trackedDiff = full.stdout.trimEnd()
    trackedTruncated = full.truncated
  }

  const untrackedArgs = headExists
    ? ['ls-files', '--others', '--exclude-standard', '-z', ...pathspec(input.scope)]
    : ['ls-files', '--cached', '--others', '--exclude-standard', '-z', ...pathspec(input.scope)]
  const untrackedResult = await runGit(input.repoRoot, untrackedArgs, {
    maxOutputBytes: 2 * 1024 * 1024
  })
  const expanded = await expandUntrackedFiles({
    repoRoot: input.repoRoot,
    paths: splitNul(untrackedResult.stdout),
    emptyFile: input.emptyFile
  })

  const own: RepositoryDiff = {
    label: input.label,
    root: input.repoRoot,
    changes: [...trackedChanges, ...expanded.changes],
    diff: [trackedDiff, ...expanded.patches].filter(Boolean).join('\n\n'),
    truncated: trackedTruncated || untrackedResult.truncated || expanded.truncated
  }
  const results = [own]

  for (const path of await listSubmodules(input.repoRoot)) {
    const childScope = submoduleScope(input.scope, path)
    if (childScope === null) continue
    const childRoot = resolve(input.repoRoot, path)
    try {
      const topLevel = (await runGit(childRoot, ['rev-parse', '--show-toplevel'], { maxOutputBytes: 16 * 1024 }))
        .stdout.trim()
      if (!topLevel) continue
      results.push(
        ...(await collectRepositoryDiff({
          repoRoot: topLevel,
          label: input.label === '.' ? path : `${input.label}/${path}`,
          ...(childScope ? { scope: childScope } : {}),
          emptyFile: input.emptyFile
        }))
      )
    } catch {
      // Uninitialized submodules have no worktree to inspect.
    }
  }

  return results
}

async function resolveCommit(repoRoot: string, ref: string): Promise<string> {
  const result = await runGit(repoRoot, ['rev-parse', '--verify', `${ref}^{commit}`], {
    maxOutputBytes: 4096
  })
  const oid = result.stdout.trim()
  if (!/^[0-9a-f]{40,64}$/i.test(oid)) throw new Error(`无法解析提交引用：${ref}`)
  return oid.toLowerCase()
}

async function firstParent(repoRoot: string, commitOid: string): Promise<string | undefined> {
  const result = await runGit(repoRoot, ['rev-parse', '--verify', '--quiet', `${commitOid}^1^{commit}`], {
    allowedExitCodes: [0, 1, 128],
    maxOutputBytes: 4096
  })
  const oid = result.stdout.trim()
  return /^[0-9a-f]{40,64}$/i.test(oid) ? oid.toLowerCase() : undefined
}

async function collectCommittedDiff(input: {
  repoRoot: string
  scope?: string
  source: Exclude<ParsedDiffArgs['source'], { kind: 'working' }>
}): Promise<{ repositories: RepositoryDiff[]; source: ResolvedDiffSource }> {
  const commonDiffArgs = ['--no-ext-diff', '--no-textconv', '--find-renames']
  let baseOid: string | undefined
  let headOid: string
  let nameArgs: string[]
  let patchArgs: string[]
  let source: ResolvedDiffSource

  if (input.source.kind === 'commit') {
    headOid = await resolveCommit(input.repoRoot, input.source.ref)
    baseOid = await firstParent(input.repoRoot, headOid)
    if (baseOid) {
      nameArgs = ['diff', ...commonDiffArgs, '--name-status', '-z', baseOid, headOid]
      patchArgs = ['diff', ...commonDiffArgs, '--no-color', baseOid, headOid]
    } else {
      nameArgs = [
        'diff-tree',
        '--root',
        '--no-commit-id',
        '-r',
        ...commonDiffArgs,
        '--name-status',
        '-z',
        headOid
      ]
      patchArgs = [
        'diff-tree',
        '--root',
        '--no-commit-id',
        '-r',
        '-p',
        ...commonDiffArgs,
        '--no-color',
        headOid
      ]
    }
    source = {
      kind: 'commit',
      label: `提交 ${input.source.ref}`,
      requestedRef: input.source.ref,
      ...(baseOid ? { baseOid } : {}),
      headOid
    }
  } else {
    baseOid = await resolveCommit(input.repoRoot, input.source.base)
    headOid = await resolveCommit(input.repoRoot, input.source.head)
    nameArgs = ['diff', ...commonDiffArgs, '--name-status', '-z', baseOid, headOid]
    patchArgs = ['diff', ...commonDiffArgs, '--no-color', baseOid, headOid]
    source = {
      kind: 'range',
      label: `${input.source.base}..${input.source.head}`,
      requestedBase: input.source.base,
      requestedHead: input.source.head,
      baseOid,
      headOid
    }
  }

  const scopedPathspec = pathspec(input.scope)
  const names = await runGit(input.repoRoot, [...nameArgs, ...scopedPathspec])
  const changes = parseNameStatus(names.stdout)
  const statCommand =
    input.source.kind === 'commit' && !baseOid
      ? [
          'diff-tree',
          '--root',
          '--no-commit-id',
          '-r',
          ...commonDiffArgs,
          '--numstat',
          '-z',
          headOid,
          ...scopedPathspec
        ]
      : [
          'diff',
          ...commonDiffArgs,
          '--numstat',
          '-z',
          baseOid!,
          headOid,
          ...scopedPathspec
        ]
  const stats = await runGit(input.repoRoot, statCommand)
  applyNumStats(changes, parseNumStat(stats.stdout))

  const sensitivePaths = changes
    .filter((change) => change.sensitive)
    .flatMap((change) => [change.path, ...(change.oldPath ? [change.oldPath] : [])])
  const patch = await runGit(input.repoRoot, [
    ...patchArgs,
    ...pathspec(input.scope, sensitivePaths)
  ])
  for (const change of changes) {
    if (change.sensitive) change.contentOmitted = '敏感文件内容已隐藏'
  }
  return {
    repositories: [
      {
        label: '.',
        root: input.repoRoot,
        changes,
        diff: patch.stdout.trimEnd(),
        truncated: patch.truncated
      }
    ],
    source
  }
}

function changeDisplayPath(label: string, change: GitChange): string {
  const path = label === '.' ? change.path : `${label}/${change.path}`
  if (!change.oldPath) return path
  const oldPath = label === '.' ? change.oldPath : `${label}/${change.oldPath}`
  return `${oldPath} -> ${path}`
}

function summarize(repositories: RepositoryDiff[]): {
  files: number
  additions: number
  deletions: number
  binaries: number
  sensitive: number
  omitted: number
  truncated: boolean
} {
  const changes = repositories.flatMap((repo) => repo.changes)
  return {
    files: changes.length,
    additions: changes.reduce((sum, change) => sum + (change.additions ?? 0), 0),
    deletions: changes.reduce((sum, change) => sum + (change.deletions ?? 0), 0),
    binaries: changes.filter((change) => change.additions === null && change.deletions === null).length,
    sensitive: changes.filter((change) => change.sensitive).length,
    omitted: changes.filter((change) => change.contentOmitted).length,
    truncated: repositories.some((repo) => repo.truncated)
  }
}

function summaryText(
  repoName: string,
  repositories: RepositoryDiff[],
  statOnly: boolean,
  sourceLabel = '当前未提交改动'
): string {
  const summary = summarize(repositories)
  if (summary.files === 0) {
    return sourceLabel === '当前未提交改动'
      ? `仓库 ${repoName} 当前没有未提交改动。`
      : `仓库 ${repoName} 的${sourceLabel}没有代码变化。`
  }

  const lines = [
    sourceLabel === '当前未提交改动'
      ? `仓库 ${repoName} 当前有 ${summary.files} 个未提交文件，+${summary.additions} / -${summary.deletions}。`
      : `仓库 ${repoName} · ${sourceLabel}：${summary.files} 个文件，+${summary.additions} / -${summary.deletions}。`,
    ...repositories.flatMap((repo) =>
      repo.changes.map((change) => {
        const stat =
          change.additions === null || change.deletions === null
            ? ''
            : ` (+${change.additions}/-${change.deletions})`
        const note = change.sensitive
          ? ' [内容已隐藏]'
          : change.contentOmitted
            ? ` [${change.contentOmitted}]`
            : ''
        return `${change.status.padEnd(4)} ${changeDisplayPath(repo.label, change)}${stat}${note}`
      })
    ).slice(0, MAX_SUMMARY_FILES)
  ]
  if (summary.files > MAX_SUMMARY_FILES) {
    lines.push(`还有 ${summary.files - MAX_SUMMARY_FILES} 个文件未在消息中展开。`)
  }
  if (!statOnly) lines.push('完整 Diff 已生成，将作为可交互预览的 HTML 附件发送。')
  if (summary.truncated) lines.push('Diff 内容超过限制，附件中已截断。')
  const text = lines.join('\n')
  return text.length <= MAX_SUMMARY_CHARS
    ? text
    : `${text.slice(0, MAX_SUMMARY_CHARS - 32).trimEnd()}\n...摘要已截断`
}

interface HtmlDiffLine {
  kind: 'add' | 'del' | 'context' | 'hunk'
  text: string
  oldLine?: number
  newLine?: number
}

interface HtmlDiffFile {
  path: string
  header: string[]
  lines: HtmlDiffLine[]
}

type HtmlPairedRow =
  | { kind: 'pair'; left?: HtmlDiffLine; right?: HtmlDiffLine }
  | { kind: 'hunk'; text: string }

function escapeHtml(value: string): string {
  return value
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;')
}

function parseHtmlDiff(text: string): HtmlDiffFile[] {
  const files: HtmlDiffFile[] = []
  let current: HtmlDiffFile | null = null
  let oldLine = 0
  let newLine = 0
  for (const raw of text.split('\n')) {
    if (raw.startsWith('diff --git ')) {
      const match = /^diff --git a\/(.+?) b\/(.+)$/.exec(raw)
      current = { path: match?.[2] ?? raw, header: [raw], lines: [] }
      files.push(current)
      oldLine = 0
      newLine = 0
      continue
    }
    if (!current) continue
    if (raw.startsWith('@@')) {
      const match = /^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/.exec(raw)
      if (match) {
        oldLine = Number.parseInt(match[1]!, 10)
        newLine = Number.parseInt(match[2]!, 10)
      }
      current.lines.push({ kind: 'hunk', text: raw })
      continue
    }
    if (
      raw.startsWith('+++ ') ||
      raw.startsWith('--- ') ||
      raw.startsWith('index ') ||
      raw.startsWith('new file') ||
      raw.startsWith('deleted file') ||
      raw.startsWith('similarity ') ||
      raw.startsWith('rename ') ||
      raw.startsWith('Binary files ')
    ) {
      current.header.push(raw)
      continue
    }
    if (raw.startsWith('+')) {
      current.lines.push({ kind: 'add', text: raw.slice(1), newLine })
      newLine += 1
    } else if (raw.startsWith('-')) {
      current.lines.push({ kind: 'del', text: raw.slice(1), oldLine })
      oldLine += 1
    } else if (raw.length === 0 || raw.startsWith(' ')) {
      current.lines.push({
        kind: 'context',
        text: raw.length > 0 ? raw.slice(1) : '',
        oldLine,
        newLine
      })
      oldLine += 1
      newLine += 1
    }
  }
  return files
}

function pairHtmlDiffLines(lines: HtmlDiffLine[]): HtmlPairedRow[] {
  const rows: HtmlPairedRow[] = []
  const deletions: HtmlDiffLine[] = []
  const additions: HtmlDiffLine[] = []
  const flush = (): void => {
    const count = Math.max(deletions.length, additions.length)
    for (let index = 0; index < count; index += 1) {
      rows.push({ kind: 'pair', left: deletions[index], right: additions[index] })
    }
    deletions.length = 0
    additions.length = 0
  }
  for (const line of lines) {
    if (line.kind === 'del') deletions.push(line)
    else if (line.kind === 'add') additions.push(line)
    else if (line.kind === 'hunk') {
      flush()
      rows.push({ kind: 'hunk', text: line.text })
    } else {
      flush()
      rows.push({ kind: 'pair', left: line, right: line })
    }
  }
  flush()
  return rows
}

function htmlLineNumber(value?: number): string {
  return value === undefined ? '' : String(value)
}

function renderSplitSide(line: HtmlDiffLine | undefined, side: 'left' | 'right'): string {
  const kind = line?.kind ?? 'empty'
  const lineNumber = side === 'left' ? line?.oldLine : line?.newLine
  const background = kind === 'add' ? '#dafbe1' : kind === 'del' ? '#ffebe9' : kind === 'empty' ? '#f6f8fa' : '#ffffff'
  return `<td class="ln ${kind}" width="54" align="right" bgcolor="${background}" style="padding:3px 8px;border-right:1px solid #d0d7de;color:#656d76">${htmlLineNumber(lineNumber)}</td><td class="code ${kind}" bgcolor="${background}" style="padding:3px 8px"><span class="code-text">${line ? escapeHtml(line.text) : ''}</span></td>`
}

function renderHtmlDiffFile(file: HtmlDiffFile, label: string, sectionLabel?: string,
  anchorId: string): string {
  const pairedRows = pairHtmlDiffLines(file.lines)
  const splitRows = pairedRows
    .map((row) =>
      row.kind === 'hunk'
        ? `<tr><td class="hunk" colspan="4" bgcolor="#ddf4ff" style="padding:5px 12px;color:#0969da">${escapeHtml(row.text)}</td></tr>`
        : `<tr>${renderSplitSide(row.left, 'left')}${renderSplitSide(row.right, 'right')}</tr>`
    )
    .join('')
  const unifiedRows = file.lines
    .map((line) => {
      if (line.kind === 'hunk') return `<div class="unified-hunk">${escapeHtml(line.text)}</div>`
      const prefix = line.kind === 'add' ? '+' : line.kind === 'del' ? '-' : ' '
      return `<div class="unified-row ${line.kind}"><span class="u-ln">${htmlLineNumber(line.oldLine)}</span><span class="u-ln">${htmlLineNumber(line.newLine)}</span><pre>${prefix}${escapeHtml(line.text)}</pre></div>`
    })
    .join('')
  const header = file.header.map(escapeHtml).join('<br>')
  return `<div class="file" id="${anchorId}"><a name="${anchorId}"></a><div class="file-title"><span>${sectionLabel ? `<small>${escapeHtml(sectionLabel)}</small>` : ''}${escapeHtml(label)}</span></div><div class="meta"><code>${header}</code></div><!-- MAICHAT_SPLIT_START --><div class="split"><table class="split-table" width="100%" border="0" cellpadding="0" cellspacing="0"><tbody>${splitRows}</tbody></table></div><!-- MAICHAT_SPLIT_END --><!-- MAICHAT_UNIFIED_START --><div class="unified">${unifiedRows}</div><!-- MAICHAT_UNIFIED_END --></div>`
}

function truncateRepositoriesForHtml(repositories: RepositoryDiff[]): {
  repositories: RepositoryDiff[]
  truncated: boolean
} {
  let remaining = MAX_HTML_DIFF_SOURCE_BYTES
  let truncated = false
  const next = repositories.map((repo) => {
    const bytes = Buffer.from(repo.diff, 'utf8')
    if (bytes.byteLength <= remaining) {
      remaining -= bytes.byteLength
      return repo
    }
    truncated = true
    const kept = remaining > 0 ? bytes.subarray(0, remaining).toString('utf8') : ''
    remaining = 0
    return { ...repo, diff: kept, truncated: true }
  })
  return { repositories: next, truncated }
}

function buildHtml(input: {
  repoName: string
  scope?: string
  repositories: RepositoryDiff[]
  source: ResolvedDiffSource
  generatedAt: number
}): { html: string; complete: boolean } {
  const limited = truncateRepositoriesForHtml(input.repositories)
  const summary = summarize(limited.repositories)
  // 先把所有文件摊平并编号，索引和正文用同一份编号，避免两边各数一次而对不齐。
  const indexedFiles = limited.repositories.flatMap((repo) =>
    parseHtmlDiff(repo.diff).map((file) => ({
      file,
      label: repo.label === '.' ? file.path : `${repo.label}/${file.path}`,
      sectionLabel: repo.label === '.' ? undefined : `Submodule: ${repo.label}`
    }))
  )
  const fileSections = indexedFiles.map(({ file, label, sectionLabel }, index) =>
    renderHtmlDiffFile(file, label, sectionLabel, `f${index}`)
  )
  // 文件索引：21 个文件平铺时，没有它就只能从头滚到尾。
  // 纯 HTML 锚点，不用 JS——桌面端是 Qt 富文本渲染，JS 根本不执行；
  // 手机端是真浏览器，但三端共用同一份 HTML，只能按最弱的那端设计。
  // （Qt 的 scrollToAnchor / setSource("#id") 已用探针实测可用，含 openExternalLinks=true 的配置。）
  const fileIndexRows = indexedFiles
    .map(({ file, label }, index) => {
      let additions = 0
      let deletions = 0
      for (const line of file.lines) {
        if (line.kind === 'add') additions += 1
        else if (line.kind === 'del') deletions += 1
      }
      return `<li><a href="#f${index}">${escapeHtml(label)}</a><span class="idx-stat"> &nbsp;&nbsp;<span class="idx-add">+${additions}</span> <span class="idx-del">-${deletions}</span></span></li>`
    })
    .join('')
  const fileIndex = fileIndexRows
    ? `<div class="file-index"><div class="file-index-title">变更文件（${indexedFiles.length}）</div><ul>${fileIndexRows}</ul></div>`
    : ''
  const omittedRows = limited.repositories.flatMap((repo) =>
    repo.changes
      .filter((change) => change.sensitive || change.contentOmitted)
      .map((change) => {
        const path = changeDisplayPath(repo.label, change)
        const reason = change.sensitive ? '敏感内容已隐藏' : change.contentOmitted ?? '内容未展开'
        return `<li><code>${escapeHtml(path)}</code> — ${escapeHtml(reason)}</li>`
      })
  )
  const complete = !summary.truncated && !limited.truncated
  const sourceDetail =
    input.source.kind === 'working'
      ? '工作区相对 HEAD'
      : input.source.kind === 'commit'
        ? `提交 ${input.source.requestedRef ?? input.source.headOid}`
        : `${input.source.requestedBase ?? input.source.baseOid}..${input.source.requestedHead ?? input.source.headOid}`
  const html = `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>${escapeHtml(input.repoName)} Diff</title>
<style>
:root{color-scheme:light dark;--bg:#f6f8fa;--panel:#fff;--border:#d0d7de;--text:#1f2328;--muted:#656d76;--add:#dafbe1;--del:#ffebe9;--hunk:#ddf4ff;--add-strong:#aceebb;--del-strong:#ffcecb}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}.page{max-width:1800px;margin:auto;padding:18px}.title{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.title h1{font-size:20px;margin:0}.pill{border:1px solid var(--border);border-radius:999px;padding:3px 9px;background:var(--panel);color:var(--muted)}.qt-separator{display:none}.summary{margin:12px 0 18px;color:var(--muted)}.warning{padding:10px 12px;background:#fff8c5;border:1px solid #d4a72c;border-radius:8px;color:#633c01}.file{margin:12px 0;border:1px solid var(--border);border-radius:9px;overflow:hidden;background:var(--panel)}.file summary{cursor:pointer;display:flex;justify-content:space-between;gap:12px;padding:11px 14px;font:600 14px ui-monospace,SFMono-Regular,Menlo,monospace;background:#f6f8fa}.file summary small{display:block;color:var(--muted);font:11px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}.meta{margin:0;padding:8px 12px;border-top:1px solid var(--border);border-bottom:1px solid var(--border);color:var(--muted);overflow:auto;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}.split-row{display:grid;grid-template-columns:54px minmax(320px,1fr) 54px minmax(320px,1fr);min-width:780px}.ln,.code{margin:0;min-height:24px;padding:3px 8px;border-bottom:1px solid rgba(208,215,222,.45);font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}.ln{text-align:right;color:var(--muted);user-select:none;border-right:1px solid var(--border)}.code{white-space:pre;overflow:hidden}.ln.add,.code.add{background:var(--add)}.ln.del,.code.del{background:var(--del)}.ln.empty,.code.empty{background:rgba(175,184,193,.12)}.hunk,.unified-hunk{padding:5px 12px;background:var(--hunk);color:#0969da;font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;border-bottom:1px solid var(--border)}.hunk{grid-column:1/-1}.split{overflow:auto}.unified{display:none}.unified-row{display:grid;grid-template-columns:44px 44px minmax(320px,1fr)}.unified-row>*{margin:0;padding:3px 7px;border-bottom:1px solid rgba(208,215,222,.45);font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}.unified-row pre{white-space:pre}.unified-row.add{background:var(--add)}.unified-row.del{background:var(--del)}.u-ln{text-align:right;color:var(--muted);border-right:1px solid var(--border);user-select:none}.omitted{color:var(--muted)}code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}@media(max-width:760px){.page{padding:10px}.split{display:none}.unified{display:block;overflow:auto}.file summary{font-size:12px}.summary{font-size:12px}}@media(prefers-color-scheme:dark){:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--text:#e6edf3;--muted:#8b949e;--add:#12261e;--del:#2d1518;--hunk:#0c2d42}.file summary{background:#161b22}.warning{background:#3b2e00;color:#f2cc60}}
.file-index{margin:0 0 16px;padding:12px 14px;border:1px solid #d0d7de;border-radius:9px;background:#fff}.file-index-title{font-weight:700;color:#1f2328;margin-bottom:8px}.file-index ul{margin:0;padding-left:18px}.file-index li{margin:2px 0;font:12px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace}.file-index a{color:#0969da;text-decoration:none}.idx-stat{margin-left:8px}.idx-add{color:#1a7f37}.idx-del{color:#cf222e}@media(prefers-color-scheme:dark){.file-index{background:#161b22;border-color:#30363d}.file-index-title{color:#e6edf3}.file-index a{color:#4493f8}.idx-add{color:#3fb950}.idx-del{color:#f85149}}.file-title{display:flex;justify-content:space-between;gap:12px;padding:11px 14px;font:600 14px ui-monospace,SFMono-Regular,Menlo,monospace;background:#f6f8fa}.file-title small{display:block;color:#656d76;font:11px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}.split-table{width:100%;min-width:780px;border-collapse:collapse;table-layout:fixed}.split-table .ln{width:54px}.split-table .code{width:calc(50% - 54px);text-align:left}.split-table .code-text{white-space:pre-wrap;overflow-wrap:anywhere;word-break:break-all;font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}.split-table .add{background:#dafbe1}.split-table .del{background:#ffebe9}.split-table .empty{background:#f6f8fa}.split-table .context{background:#fff}.split-table .hunk{background:#ddf4ff;color:#0969da;text-align:left}@media(max-width:760px){.split-table{display:none}}@media(prefers-color-scheme:dark){.file-title{background:#161b22}.file-title small{color:#8b949e}.split-table .add{background:#12261e}.split-table .del{background:#2d1518}.split-table .empty,.split-table .context{background:#161b22}.split-table .hunk{background:#0c2d42}}
</style></head><body><main class="page"><div class="title"><h1>${escapeHtml(input.repoName)}</h1><span class="pill">${escapeHtml(sourceDetail)}</span><span class="qt-separator" aria-hidden="true"> · </span><span class="pill">${summary.files} files</span><span class="qt-separator" aria-hidden="true"> · </span><span class="pill">+${summary.additions} / -${summary.deletions}</span></div><div class="summary">范围：<code>${escapeHtml(input.scope || '全部')}</code> · 生成：${escapeHtml(new Date(input.generatedAt).toISOString())}</div>${complete ? '' : '<p class="warning">报告内容超过安全预览上限，以下 Diff 不完整。请按文件路径重新请求。</p>'}${fileIndex}${omittedRows.length ? `<div class="omitted"><strong>未展开：</strong><ul>${omittedRows.join('')}</ul></div>` : ''}${fileSections.join('') || '<p>没有可展示的文本 Diff。</p>'}</main></body></html>`
  return { html, complete }
}

async function cleanupReports(outputDir: string, now: number): Promise<void> {
  let entries
  try {
    entries = await fs.readdir(outputDir, { withFileTypes: true })
  } catch {
    return
  }
  const reports: Array<{ path: string; mtimeMs: number }> = []
  for (const entry of entries) {
    if (
      !entry.isFile() ||
      !entry.name.startsWith('remote-im-diff-') ||
      (!entry.name.endsWith('.md') && !entry.name.endsWith('.html'))
    ) {
      continue
    }
    const path = join(outputDir, entry.name)
    try {
      const stat = await fs.stat(path)
      reports.push({ path, mtimeMs: stat.mtimeMs })
    } catch {
      // Ignore files removed concurrently.
    }
  }
  reports.sort((a, b) => b.mtimeMs - a.mtimeMs)
  await Promise.all(
    reports
      .filter((report, index) => index >= MAX_STORED_REPORTS || now - report.mtimeMs > REPORT_MAX_AGE_MS)
      .map((report) => fs.rm(report.path, { force: true }))
  )
}

export async function createGitDiffReport(
  input: CreateGitDiffReportInput
): Promise<CreateGitDiffReportResult> {
  const parsed = parseDiffArgs(input.args)
  if ('error' in parsed) return { ok: false, error: parsed.error, text: parsed.error }

  const now = input.now?.() ?? Date.now()
  let repoRoot: string
  try {
    repoRoot = (
      await runGit(input.targetRepo, ['rev-parse', '--show-toplevel'], {
        maxOutputBytes: 16 * 1024
      })
    ).stdout.trim()
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    return { ok: false, error: message, text: `无法读取 Git 仓库：${message}` }
  }
  if (!repoRoot) {
    return { ok: false, error: 'Git repository root is empty', text: '无法确定 Git 仓库根目录。' }
  }
  try {
    repoRoot = await fs.realpath(repoRoot)
  } catch {
    // Keep Git's path when canonicalization is unavailable (for example on a
    // network drive that disappeared after rev-parse).
  }

  let scope: string | undefined
  if (parsed.scope) {
    let targetRepo = input.targetRepo
    try {
      targetRepo = await fs.realpath(targetRepo)
    } catch {
      // The Git command above already validated the working directory.
    }
    const absoluteScope = resolve(targetRepo, parsed.scope)
    if (!isInside(repoRoot, absoluteScope)) {
      return {
        ok: false,
        error: 'diff path is outside the repository',
        text: '只能查看当前仓库内文件的 Diff。'
      }
    }
    scope = normalizeGitPath(relative(repoRoot, absoluteScope)) || '.'
  }

  const temporaryDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-git-diff-'))
  const emptyFile = join(temporaryDir, 'empty')
  try {
    await fs.writeFile(emptyFile, '')
    let repositories: RepositoryDiff[]
    let source: ResolvedDiffSource
    if (parsed.source.kind === 'working') {
      repositories = await collectRepositoryDiff({
        repoRoot,
        label: '.',
        ...(scope ? { scope } : {}),
        emptyFile
      })
      let headOid = 'unborn'
      if (await hasHead(repoRoot)) headOid = await resolveCommit(repoRoot, 'HEAD')
      source = { kind: 'working', label: '当前未提交改动', headOid }
    } else {
      const collected = await collectCommittedDiff({
        repoRoot,
        ...(scope ? { scope } : {}),
        source: parsed.source
      })
      repositories = collected.repositories
      source = collected.source
    }
    const text = summaryText(
      basename(repoRoot),
      repositories,
      parsed.statOnly,
      source.label
    )
    if (repositories.every((repo) => repo.changes.length === 0) || parsed.statOnly) {
      return { ok: true, text }
    }

    await fs.mkdir(input.outputDir, { recursive: true })
    await cleanupReports(input.outputDir, now)
    const safeRepoName = basename(repoRoot).replace(/[^A-Za-z0-9._-]+/g, '-') || 'repo'
    const artifactID = randomUUID()
    const rendered = buildHtml({
      repoName: basename(repoRoot),
      ...(scope ? { scope } : {}),
      repositories,
      source,
      generatedAt: now
    })
    const bytes = Buffer.from(rendered.html, 'utf8')
    if (bytes.byteLength > MAX_REPORT_BYTES) {
      throw new Error('生成的 HTML Diff 超过安全预览上限，请限定文件路径后重试')
    }
    const sha256 = createHash('sha256').update(bytes).digest('hex')
    const attachmentPath = join(
      input.outputDir,
      `remote-im-diff-${safeRepoName}-${new Date(now).toISOString().replace(/[:.]/g, '-')}-${sha256}.html`
    )
    await fs.writeFile(attachmentPath, bytes)
    await cleanupReports(input.outputDir, now)
    const totals = summarize(repositories)
    return {
      ok: true,
      text,
      attachmentPath,
      artifact: {
        schema: 'git-diff/v1',
        id: artifactID,
        repositoryName: basename(repoRoot),
        source,
        files: totals.files,
        additions: totals.additions,
        deletions: totals.deletions,
        sha256,
        sizeBytes: bytes.byteLength,
        complete: rendered.complete
      }
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    return { ok: false, error: message, text: `生成 Git Diff 失败：${message}` }
  } finally {
    await fs.rm(temporaryDir, { recursive: true, force: true }).catch(() => {})
  }
}
