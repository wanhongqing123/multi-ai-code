import { spawn } from 'node:child_process'
import { randomUUID } from 'node:crypto'
import { promises as fs } from 'node:fs'
import { tmpdir } from 'node:os'
import { basename, isAbsolute, join, relative, resolve, sep } from 'node:path'

const COMMAND_TIMEOUT_MS = 20_000
const MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
const MAX_REPORT_BYTES = 4_500_000
const MAX_UNTRACKED_FILE_BYTES = 512 * 1024
const MAX_EXPANDED_UNTRACKED_FILES = 100
const MAX_SUMMARY_FILES = 40
const MAX_SUMMARY_CHARS = 6000
const MAX_STORED_REPORTS = 20
const REPORT_MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000

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
}

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

  for (;;) {
    const match = /^(--stat|--all)(?:\s+|$)/.exec(remaining)
    if (!match) break
    if (match[1] === '--stat') statOnly = true
    if (match[1] === '--all') all = true
    remaining = remaining.slice(match[0].length).trim()
  }

  if (remaining.startsWith('--')) {
    return { error: '用法：/diff [--stat] [文件或目录]' }
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

function displayPath(path: string): string {
  return path.replaceAll('\\', '\\\\').replaceAll('\n', '\\n').replaceAll('\r', '\\r')
}

function markdownCell(value: string): string {
  return displayPath(value).replaceAll('|', '\\|').replaceAll('`', '\\`')
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

function summaryText(repoName: string, repositories: RepositoryDiff[], statOnly: boolean): string {
  const summary = summarize(repositories)
  if (summary.files === 0) return `仓库 ${repoName} 当前没有未提交改动。`

  const lines = [
    `仓库 ${repoName} 当前有 ${summary.files} 个未提交文件，+${summary.additions} / -${summary.deletions}。`,
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
  if (!statOnly) lines.push('完整 Diff 已生成，将作为 Markdown 附件发送。')
  if (summary.truncated) lines.push('Diff 内容超过限制，附件中已截断。')
  const text = lines.join('\n')
  return text.length <= MAX_SUMMARY_CHARS
    ? text
    : `${text.slice(0, MAX_SUMMARY_CHARS - 32).trimEnd()}\n...摘要已截断`
}

function buildMarkdown(input: {
  repoName: string
  scope?: string
  repositories: RepositoryDiff[]
  generatedAt: number
}): string {
  const summary = summarize(input.repositories)
  const rows = input.repositories.flatMap((repo) =>
    repo.changes.map((change) => {
      const additions = change.additions === null ? '-' : String(change.additions)
      const deletions = change.deletions === null ? '-' : String(change.deletions)
      const note = change.sensitive ? '敏感内容已隐藏' : change.contentOmitted ?? ''
      return `| ${markdownCell(change.status)} | ${markdownCell(changeDisplayPath(repo.label, change))} | ${additions} | ${deletions} | ${markdownCell(note)} |`
    })
  )

  const sections = input.repositories
    .filter((repo) => repo.diff.trim())
    .map(
      (repo) =>
        `## ${repo.label === '.' ? '主仓库' : `Submodule: ${repo.label}`}\n\n\`\`\`diff\n${repo.diff}\n\`\`\``
    )
  const notes = [
    summary.sensitive > 0 ? `- ${summary.sensitive} 个敏感文件未包含具体内容。` : '',
    summary.omitted > 0 ? `- ${summary.omitted} 个文件因类型或大小未展开。` : '',
    summary.truncated ? '- 报告内容超过上限，部分 Diff 已截断。' : ''
  ].filter(Boolean)

  const markdown = [
    '# Repository Diff',
    '',
    `- 仓库：\`${input.repoName}\``,
    `- 范围：\`${input.scope || '全部未提交改动'}\``,
    `- 生成时间：${new Date(input.generatedAt).toISOString()}`,
    `- 汇总：${summary.files} files, +${summary.additions} / -${summary.deletions}`,
    ...notes,
    '',
    '## 文件列表',
    '',
    '| 状态 | 文件 | 新增 | 删除 | 说明 |',
    '| --- | --- | ---: | ---: | --- |',
    ...rows,
    '',
    ...sections
  ].join('\n')

  const bytes = Buffer.from(markdown, 'utf8')
  if (bytes.byteLength <= MAX_REPORT_BYTES) return markdown
  const prefix = bytes.subarray(0, MAX_REPORT_BYTES - 160).toString('utf8').trimEnd()
  return `${prefix}\n\n\`\`\`\n\n> 报告超过大小限制，剩余 Diff 已截断。\n`
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
    if (!entry.isFile() || !entry.name.startsWith('remote-im-diff-') || !entry.name.endsWith('.md')) {
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
    const repositories = await collectRepositoryDiff({
      repoRoot,
      label: '.',
      ...(scope ? { scope } : {}),
      emptyFile
    })
    const text = summaryText(basename(repoRoot), repositories, parsed.statOnly)
    if (repositories.every((repo) => repo.changes.length === 0) || parsed.statOnly) {
      return { ok: true, text }
    }

    await fs.mkdir(input.outputDir, { recursive: true })
    await cleanupReports(input.outputDir, now)
    const safeRepoName = basename(repoRoot).replace(/[^A-Za-z0-9._-]+/g, '-') || 'repo'
    const attachmentPath = join(
      input.outputDir,
      `remote-im-diff-${safeRepoName}-${new Date(now).toISOString().replace(/[:.]/g, '-')}-${randomUUID().slice(0, 8)}.md`
    )
    await fs.writeFile(
      attachmentPath,
      buildMarkdown({
        repoName: basename(repoRoot),
        ...(scope ? { scope } : {}),
        repositories,
        generatedAt: now
      }),
      'utf8'
    )
    await cleanupReports(input.outputDir, now)
    return { ok: true, text, attachmentPath }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    return { ok: false, error: message, text: `生成 Git Diff 失败：${message}` }
  } finally {
    await fs.rm(temporaryDir, { recursive: true, force: true }).catch(() => {})
  }
}
