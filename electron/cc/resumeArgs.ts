/**
 * Builds CLI args for resuming a previous interactive session.
 *
 * - claude: prepends `--continue`, removes any conflicting continue/resume flags
 *   (and their value, for the value-taking ones) from the original args.
 * - codex: prepends the `resume --last` subcommand and removes any pre-existing
 *   `resume` / `fork` subcommand the caller may have set.
 * - opencode: prepends `--continue`（按当前项目目录恢复最近会话），removes any
 *   conflicting `--continue/-c`、`--fork` 及带值的 `--session/-s`。
 *
 * Non-conflicting flags (e.g. `--model`, `--effort`) are preserved verbatim.
 */
export type ResumeCommand = 'claude' | 'codex' | 'opencode'

const CLAUDE_CONTINUE_FLAGS = new Set(['--continue', '-c'])
// Flags that take a positional value following them; the value must be dropped too.
const CLAUDE_RESUME_VALUE_FLAGS = new Set(['--resume', '-r'])
// `--fork-session` is a boolean flag tied to resume; on a fresh resume start we
// drop it (caller can re-add later via a dedicated UX if ever needed).
const CLAUDE_FORK_FLAGS = new Set(['--fork-session'])

const CODEX_RESUME_SUBCOMMANDS = new Set(['resume', 'fork'])

const OPENCODE_CONTINUE_FLAGS = new Set(['--continue', '-c', '--fork'])
// 带值的会话选择 flag，值也要一并丢弃。
const OPENCODE_SESSION_VALUE_FLAGS = new Set(['--session', '-s'])

function filterClaudeArgs(args: string[]): string[] {
  const out: string[] = []
  for (let i = 0; i < args.length; i++) {
    const a = args[i]
    if (CLAUDE_CONTINUE_FLAGS.has(a)) continue
    if (CLAUDE_FORK_FLAGS.has(a)) continue
    if (CLAUDE_RESUME_VALUE_FLAGS.has(a)) {
      // Drop the flag and its value (if a value follows that doesn't itself look like a flag).
      const next = args[i + 1]
      if (next !== undefined && !next.startsWith('-')) i += 1
      continue
    }
    // `--resume=xxx` style.
    if (a.startsWith('--resume=')) continue
    out.push(a)
  }
  return out
}

function filterCodexArgs(args: string[]): string[] {
  // Codex subcommands appear as positional tokens before any flags. We strip
  // a leading `resume` or `fork` plus its immediate sub-args until the first
  // flag, since those positional args (SESSION_ID / PROMPT) belong to the
  // dropped subcommand.
  if (args.length === 0) return args
  if (!CODEX_RESUME_SUBCOMMANDS.has(args[0])) return [...args]
  let i = 1
  while (i < args.length && !args[i].startsWith('-')) i += 1
  return args.slice(i)
}

function filterOpenCodeArgs(args: string[]): string[] {
  const out: string[] = []
  for (let i = 0; i < args.length; i++) {
    const a = args[i]
    if (OPENCODE_CONTINUE_FLAGS.has(a)) continue
    if (OPENCODE_SESSION_VALUE_FLAGS.has(a)) {
      const next = args[i + 1]
      if (next !== undefined && !next.startsWith('-')) i += 1
      continue
    }
    if (a.startsWith('--session=') || a.startsWith('-s=')) continue
    out.push(a)
  }
  return out
}

export function buildResumeArgs(
  command: ResumeCommand,
  originalArgs: string[]
): string[] {
  if (command === 'claude') {
    return ['--continue', ...filterClaudeArgs(originalArgs)]
  }
  if (command === 'opencode') {
    return ['--continue', ...filterOpenCodeArgs(originalArgs)]
  }
  return ['resume', '--last', ...filterCodexArgs(originalArgs)]
}
