import { promises as fs } from 'fs'
import { join } from 'path'
import { joinWithRootStyle } from '../pathStyle.js'
import { designArchiveDir } from '../store/paths.js'

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
  return joinWithRootStyle(designArchiveDir(targetRepo), `${safe}.md`)
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

export const MAIN_COMMAND_DEFAULT = 'codex'
export type SupportedCli = 'claude' | 'codex' | 'opencode'
export const CODEX_CONTEXT_WINDOW_CONFIG = 'model_context_window=1000000'
const CODEX_NO_ALT_SCREEN_ARG = '--no-alt-screen'
const CODEX_BYPASS_HOOK_TRUST_ARG = '--dangerously-bypass-hook-trust'

function hasAnyArg(args: readonly string[], flags: readonly string[]): boolean {
  return args.some((arg) => flags.includes(arg))
}

function hasCodexContextWindowConfig(args: readonly string[]): boolean {
  return args.some((arg, index) => {
    if (arg === '-c' || arg === '--config') {
      return args[index + 1]?.startsWith('model_context_window=') === true
    }
    return arg.startsWith('-cmodel_context_window=') ||
      arg.startsWith('--config=model_context_window=')
  })
}

function codexDefaultArgs(extraArgs: readonly string[] = []): string[] {
  const args: string[] = []
  if (!hasAnyArg(extraArgs, [CODEX_NO_ALT_SCREEN_ARG])) {
    args.push(CODEX_NO_ALT_SCREEN_ARG)
  }
  if (!hasAnyArg(extraArgs, ['--dangerously-bypass-approvals-and-sandbox'])) {
    args.push('--dangerously-bypass-approvals-and-sandbox')
  }
  if (!hasAnyArg(extraArgs, [CODEX_BYPASS_HOOK_TRUST_ARG])) {
    args.push(CODEX_BYPASS_HOOK_TRUST_ARG)
  }
  if (!hasCodexContextWindowConfig(extraArgs)) {
    args.push('-c', CODEX_CONTEXT_WINDOW_CONFIG)
  }
  return args
}

function opencodeDefaultArgs(extraArgs: readonly string[] = []): string[] {
  if (hasAnyArg(extraArgs, ['--dangerously-skip-permissions', '--yolo', '--auto'])) {
    return []
  }
  return ['--dangerously-skip-permissions']
}

export function mainCliArgs(
  binary: SupportedCli = MAIN_COMMAND_DEFAULT
): string[] {
  if (binary === 'codex') return codexDefaultArgs()
  if (binary === 'opencode') return opencodeDefaultArgs()
  return ['--dangerously-skip-permissions']
}

export function buildCliLaunchArgs(
  binary: SupportedCli,
  _targetRepo: string,
  extraArgs: readonly string[] = []
): string[] {
  const args: string[] = []
  if (binary === 'claude') {
    if (!hasAnyArg(extraArgs, ['--dangerously-skip-permissions'])) {
      args.push('--dangerously-skip-permissions')
    }
    return [...args, ...extraArgs]
  }

  if (binary === 'opencode') {
    args.push(...opencodeDefaultArgs(extraArgs))
    return [...args, ...extraArgs]
  }

  args.push(...codexDefaultArgs(extraArgs))
  return [...args, ...extraArgs]
}
