import { mkdirSync, readFileSync, realpathSync, writeFileSync } from 'fs'
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from 'path'
import { homedir } from 'os'
import { isCodexCommand } from './codexConfig.js'

// Keep CLI administration/service commands on their own transport path. These
// names and aliases follow the bundled kernel's cli/src/main.rs Subcommand.
const CODEX_NON_INTERACTIVE_COMMANDS = new Set([
  'tcp-tunnel', 'exec', 'e', 'review', 'login', 'git-credential', 'logout',
  'mcp', 'plugin', 'app-server', 'remote-control', 'app', 'completion',
  'update', 'doctor', 'sandbox', 'debug', 'execpolicy', 'apply', 'a', 'queue',
  'archive', 'delete', 'migrate-rollouts', 'migrate-nfs-db', 'unarchive',
  'cloud', 'cloud-tasks', 'responses-api-proxy', 'exec-server', 'features', 'help'
])

// Skip root option values when looking for a subcommand: a model/profile/path
// named "login" is still a value, not a request to administer the CLI.
const CODEX_VALUE_OPTIONS = new Set([
  '--model', '-m', '--config', '-c', '--cd', '-C', '--profile', '-p',
  '--local-provider', '--sandbox', '-s', '--ask-for-approval', '-a',
  '--add-dir', '--enable', '--disable', '--multi-ai-code-im-ipc'
])

/**
 * Select the embedded app server for the app's bundled interactive Codex.
 * The product ships the kernel and code-mode host, not a daemon installation.
 * Resume, fork and agents accept this root flag too. Explicit remote transport
 * (which conflicts with --no-daemon) and service commands stay caller-owned.
 */
export function withEmbeddedCodexArgs(command: string, args: readonly string[]): string[] {
  if (!isCodexCommand(command)) return [...args]
  let firstPositional: string | undefined
  for (let i = 0; i < args.length; i++) {
    const arg = args[i]
    // Everything after -- is literal prompt text, including flag/command names.
    if (arg === '--') break
    const option = arg.split('=', 1)[0]
    if (['--no-daemon', '--remote', '--remote-auth-token-env', '--help', '-h', '--version', '-V']
      .includes(option)) return [...args]
    if (CODEX_VALUE_OPTIONS.has(option)) {
      if (!arg.includes('=')) i++
    } else if (arg === '--image' || arg === '-i') {
      // Split image flags consume values up to the next option. An attached
      // value (--image=path / -ipath) ends that group in upstream clap.
      while (i + 1 < args.length && !args[i + 1].startsWith('-')) i++
    } else if (!arg.startsWith('-') && firstPositional === undefined) {
      firstPositional = arg
    }
  }
  if (firstPositional && CODEX_NON_INTERACTIVE_COMMANDS.has(firstPositional)) return [...args]
  return ['--no-daemon', ...args]
}

// Resolve existing ancestors too: the requested directory may not exist yet,
// while its parent (or the home itself) can be a symlink/junction.
function comparablePath(path: string): string {
  let ancestor = resolve(path)
  const tail: string[] = []
  while (true) {
    try {
      const canonical = join(realpathSync.native(ancestor), ...tail)
      return process.platform === 'win32' || process.platform === 'darwin'
        ? canonical.toLowerCase()
        : canonical
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code
      const parent = dirname(ancestor)
      if (!['ENOENT', 'ENOTDIR'].includes(code ?? '') || parent === ancestor) throw error
      tail.unshift(basename(ancestor))
      ancestor = parent
    }
  }
}

// Inputs are already canonical paths. Compare path segments, not raw prefixes:
// `.codex-app-data` is a sibling, not a child of `.codex`.
function isWithinDirectory(parent: string, candidate: string): boolean {
  const child = relative(parent, candidate)
  return child === '' || (!isAbsolute(child) && child !== '..' && !child.startsWith(`..${sep}`))
}

/**
 * Tells Codex to stop *offering* to turn its Windows sandbox on.
 *
 * This suppresses an offer, not a protection. Codex spells "no sandbox" by
 * omitting the `[windows] sandbox` key — there is no `disabled` value, and
 * writing one stops Codex booting — but an omitted key is also half of the
 * trigger for the offer, so the same question returns for every new directory
 * and declining records nothing. This answers that one question, once.
 *
 * Scope, precisely: it changes nothing except whether the *optional* offer is
 * shown. A sandbox mode already configured stays in force; a sandbox that
 * policy requires still prompts, because that prompt is how it gets
 * provisioned. It does not disable an active sandbox, and does not mean every
 * Codex session runs unsandboxed — only that sessions with no sandbox
 * configured stop being asked to create one.
 *
 * Must match `SUPPRESS_OPTIONAL_PROMPT_ENV` in the kernel's
 * `tui/src/windows_sandbox.rs`; a test on each side pins the spelling.
 */
export const SUPPRESS_SANDBOX_PROMPT_ENV = 'CODEX_SUPPRESS_OPTIONAL_WINDOWS_SANDBOX_PROMPT'

/** All accounts share the app-owned home, never an inherited caller's home. */
export function withCodexHome(
  env: Record<string, string>,
  codexHome: string,
  workingDirectory: string
): Record<string, string> {
  if (!isAbsolute(codexHome)) throw new Error('Codex home must be an absolute path')
  const home = comparablePath(codexHome)
  if (isWithinDirectory(comparablePath(join(homedir(), '.codex')), home)) {
    throw new Error('Codex shared home must not use the host global .codex directory')
  }
  if (isWithinDirectory(comparablePath(workingDirectory), home)) {
    throw new Error('Codex shared home must be outside the target repository')
  }
  mkdirSync(codexHome, { recursive: true, mode: 0o700 })
  const next = { ...env }
  // Windows environment variable names are case-insensitive. Remove aliases
  // before injecting the canonical key so the inherited value cannot win.
  for (const key of Object.keys(next)) {
    if ([
      'CODEX_HOME',
      'CODEX_SQLITE_HOME',
      'CODEX_ANALYTICS_EVENTS_CAPTURE_FILE',
      SUPPRESS_SANDBOX_PROMPT_ENV
    ].includes(key.toUpperCase())) delete next[key]
  }
  // Do not inherit a developer capture sink that writes outside this home.
  // Managed policy and CA settings are intentionally retained, not bypassed.
  next.CODEX_HOME = codexHome
  next.CODEX_SQLITE_HOME = codexHome
  next[SUPPRESS_SANDBOX_PROMPT_ENV] = '1'
  return next
}

/** Only touch the effective Codex home, never ~/.codex or the target repo. */
export function dismissCodexUpgradeNotice(codexHome: string): void {
  try {
    const file = join(codexHome, 'version.json')
    const state = JSON.parse(readFileSync(file, 'utf8'))
    if (state.latest_version && state.dismissed_version !== state.latest_version) {
      state.dismissed_version = state.latest_version
      writeFileSync(file, JSON.stringify(state), 'utf8')
    }
  } catch { /* Optional UI state may not exist on first run. */ }
}
