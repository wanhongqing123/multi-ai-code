export type SupportedCli = 'claude' | 'codex' | 'opencode'
export const CODEX_CONTEXT_WINDOW_CONFIG = 'model_context_window=1000000'
const CODEX_NO_ALT_SCREEN_ARG = '--no-alt-screen'

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

function codexDefaultArgs(extraArgs: readonly string[]): string[] {
  const args: string[] = []
  if (!hasAnyArg(extraArgs, [CODEX_NO_ALT_SCREEN_ARG])) {
    args.push(CODEX_NO_ALT_SCREEN_ARG)
  }
  if (!hasAnyArg(extraArgs, ['--dangerously-bypass-approvals-and-sandbox'])) {
    args.push('--dangerously-bypass-approvals-and-sandbox')
  }
  if (!hasCodexContextWindowConfig(extraArgs)) {
    args.push('-c', CODEX_CONTEXT_WINDOW_CONFIG)
  }
  return args
}

function opencodeDefaultArgs(extraArgs: readonly string[]): string[] {
  if (hasAnyArg(extraArgs, ['--dangerously-skip-permissions', '--yolo', '--auto'])) {
    return []
  }
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
