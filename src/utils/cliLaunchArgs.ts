export type SupportedCli = 'claude' | 'codex'

function hasAnyArg(args: readonly string[], flags: readonly string[]): boolean {
  return args.some((arg) => flags.includes(arg))
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

  if (
    !hasAnyArg(extraArgs, [
      '--dangerously-bypass-approvals-and-sandbox'
    ])
  ) {
    args.push('--dangerously-bypass-approvals-and-sandbox')
  }
  return [...args, ...extraArgs]
}
