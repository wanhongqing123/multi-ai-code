import { mkdirSync, readFileSync, realpathSync, writeFileSync } from 'fs'
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from 'path'
import { homedir } from 'os'

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
 * and declining records nothing. This app deliberately runs Codex unsandboxed,
 * so it answers once, here. A sandbox that policy *requires* still prompts.
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
