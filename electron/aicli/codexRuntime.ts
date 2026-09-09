import { mkdirSync, readFileSync, writeFileSync } from 'fs'
import { isAbsolute, join } from 'path'

/** The host owns CODEX_HOME; neither a caller nor an inherited shell may share it. */
export function withCodexHome(
  env: Record<string, string>,
  codexHome: string
): Record<string, string> {
  if (!isAbsolute(codexHome)) throw new Error('Codex home must be an absolute path')
  mkdirSync(codexHome, { recursive: true, mode: 0o700 })
  const next = { ...env }
  // Windows environment variable names are case-insensitive. Remove aliases
  // before injecting the canonical key so the inherited value cannot win.
  for (const key of Object.keys(next)) {
    if (['CODEX_HOME', 'CODEX_SQLITE_HOME', 'CODEX_ANALYTICS_EVENTS_CAPTURE_FILE'].includes(key.toUpperCase())) delete next[key]
  }
  // Do not inherit a developer capture sink that writes outside this home.
  // Managed policy and CA settings are intentionally retained, not bypassed.
  next.CODEX_HOME = codexHome
  next.CODEX_SQLITE_HOME = codexHome
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
