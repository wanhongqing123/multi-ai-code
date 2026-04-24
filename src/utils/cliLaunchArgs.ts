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
