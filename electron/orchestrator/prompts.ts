import { promises as fs } from 'fs'
import { join, dirname, isAbsolute } from 'path'
import { fileURLToPath } from 'url'
import { designArchiveDir } from '../store/paths.js'

const __dirname = dirname(fileURLToPath(import.meta.url))

function sanitizeLabel(label: string): string {
  return label
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
    .replace(/\s+/g, '_')
    .slice(0, 80)
}

/**
 * Absolute path for a plan's design markdown.
 * Returns undefined when targetRepo is missing.
 */
export function planArtifactPath(
  label: string | null | undefined,
  targetRepo: string | null | undefined
): string | undefined {
  if (!targetRepo) return undefined
  const safe = label && label.trim() ? sanitizeLabel(label) : 'design'
  return join(designArchiveDir(targetRepo), `${safe}.md`)
}

/**
 * Reads `project.json` from `projectDir` to derive target_repo, then returns
 * the canonical design path. Throws if project.json is missing or malformed.
 */
export async function resolvePlanArtifactAbs(
  projectDir: string,
  label: string | null | undefined
): Promise<string> {
  const metaPath = join(projectDir, 'project.json')
  const raw = await fs.readFile(metaPath, 'utf8')
  const meta = JSON.parse(raw) as { target_repo?: string }
  if (!meta.target_repo) {
    throw new Error(`project.json missing target_repo: ${metaPath}`)
  }
  const p = planArtifactPath(label, meta.target_repo)
  if (!p) throw new Error('planArtifactPath returned undefined')
  return p
}

export const MAIN_COMMAND_DEFAULT = 'claude'
export type SupportedCli = 'claude' | 'codex'

const SAFE_READS = [
  'Read',
  'Glob',
  'Grep',
  'Bash(ls:*)',
  'Bash(cat:*)',
  'Bash(head:*)',
  'Bash(tail:*)',
  'Bash(wc:*)',
  'Bash(find:*)',
  'Bash(pwd)',
  'Bash(echo:*)'
]

const SAFE_GIT = [
  'Bash(git status:*)',
  'Bash(git log:*)',
  'Bash(git diff:*)',
  'Bash(git show:*)',
  'Bash(git blame:*)',
  'Bash(git branch:*)',
  'Bash(git remote:*)',
  'Bash(git rev-parse:*)'
]

const WRITE_TOOLS = ['Write', 'Edit', 'MultiEdit']

function hasAnyArg(args: readonly string[], flags: readonly string[]): boolean {
  return args.some((arg) => flags.includes(arg))
}

export function mainCliArgs(
  binary: SupportedCli = MAIN_COMMAND_DEFAULT
): string[] {
  if (binary === 'codex') return ['--sandbox', 'workspace-write', '-a', 'never']
  const allowed = [...SAFE_READS, ...SAFE_GIT, ...WRITE_TOOLS].join(' ')
  return ['--permission-mode', 'acceptEdits', '--allowedTools', allowed]
}

export function buildCliLaunchArgs(
  binary: SupportedCli,
  targetRepo: string,
  extraArgs: readonly string[] = []
): string[] {
  const args: string[] = []
  if (binary === 'claude') {
    if (!hasAnyArg(extraArgs, ['--add-dir'])) {
      args.push('--add-dir', targetRepo)
    }
    if (!hasAnyArg(extraArgs, ['--permission-mode'])) {
      args.push('--permission-mode', 'acceptEdits')
    }
    if (!hasAnyArg(extraArgs, ['--allowedTools', '--allowed-tools'])) {
      const allowed = [...SAFE_READS, ...SAFE_GIT, ...WRITE_TOOLS].join(' ')
      args.push('--allowedTools', allowed)
    }
    return [...args, ...extraArgs]
  }

  if (!hasAnyArg(extraArgs, ['-C', '--cd'])) {
    args.push('-C', targetRepo)
  }
  if (!hasAnyArg(extraArgs, ['--sandbox', '-s'])) {
    args.push('--sandbox', 'workspace-write')
  }
  if (!hasAnyArg(extraArgs, ['-a', '--ask-for-approval'])) {
    args.push('-a', 'never')
  }
  return [...args, ...extraArgs]
}

function promptsDir(): string {
  return join(__dirname, '..', '..', 'electron', 'prompts')
}

function fallbackPromptsDir(): string {
  return join(__dirname, 'prompts')
}

export async function loadMainPromptTemplate(): Promise<string> {
  for (const base of [promptsDir(), fallbackPromptsDir()]) {
    try {
      return await fs.readFile(join(base, 'main.md'), 'utf8')
    } catch {
      /* try next */
    }
  }
  throw new Error('prompt template not found: main.md')
}

export interface RenderContext {
  projectDir: string
  /** Absolute path to the plan markdown. */
  artifactPath: string
  projectName?: string
  targetRepo?: string
  stageCwd?: string
  /** When true, renderTemplate uses a placeholder in ARTIFACT_PATH so the
   *  CLI can ask the user to pick a plan name at archive time. */
  planPending?: boolean
}

export function renderTemplate(tpl: string, ctx: RenderContext): string {
  let artifactAbs: string
  if (ctx.planPending) {
    const root = (ctx.targetRepo ?? ctx.projectDir).replace(/[\/\\]+$/, '')
    artifactAbs = `${root}/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md`
  } else if (isAbsolute(ctx.artifactPath)) {
    artifactAbs = ctx.artifactPath
  } else {
    artifactAbs = `${ctx.projectDir.replace(/\/$/, '')}/${ctx.artifactPath}`
  }
  return tpl
    .replaceAll('{{PROJECT_DIR}}', ctx.projectDir)
    .replaceAll('{{PROJECT_NAME}}', ctx.projectName ?? '(未设置)')
    .replaceAll('{{TARGET_REPO}}', ctx.targetRepo ?? '(未设置)')
    .replaceAll('{{STAGE_CWD}}', ctx.stageCwd ?? ctx.projectDir)
    .replaceAll('{{ARTIFACT_PATH}}', artifactAbs)
}

function buildProjectContextBlock(ctx: RenderContext): string {
  return [
    '# 项目上下文（平台自动注入）',
    '',
    `- **项目名**：${ctx.projectName ?? '(未设置)'}`,
    `- **代码仓库绝对路径**：${ctx.targetRepo ?? '(未设置)'}`,
    `- **你的工作目录 (cwd)**：${ctx.stageCwd ?? ctx.projectDir}`,
    `- **项目根目录**：${ctx.projectDir}`,
    '',
    '---',
    ''
  ].join('\n')
}

export async function buildSystemPrompt(ctx: RenderContext): Promise<string> {
  const tpl = await loadMainPromptTemplate()
  const body = renderTemplate(tpl, ctx)
  return buildProjectContextBlock(ctx) + body
}
