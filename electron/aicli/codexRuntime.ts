import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs'
import { isAbsolute, join } from 'path'

/**
 * 首次为某个账号建目录时写一份**可见的**默认 config.toml。
 *
 * 为什么需要它：账号隔离之后，「Windows 沙箱已安装完成」的标记留在旧的全局目录里，
 * 新目录看不到。沙箱级别是 elevated 时 codex 会认为需要重装，去拉
 * codex-windows-sandbox-setup.exe —— 而那个 helper 当前不打包（省 23.7MB），
 * 于是 Windows 弹「找不到文件 'codex-windows-sandbox-setup.exe'」。
 * 用户明确要求不使用沙箱，而且不该由用户自己去手写这份配置。
 *
 * 这里写的是**默认值而不是强制值**：
 * - 只在文件不存在时写，绝不覆盖用户已有配置；
 * - 写明文 toml 并带注释，用户改回 unelevated / elevated 随时生效；
 * - 它降低了隔离强度，所以必须是一个**看得见、改得回**的默认，不能静默生效。
 */
export function seedAccountCodexConfig(accountHome: string): void {
  const file = join(accountHome, 'config.toml')
  if (existsSync(file)) return
  try {
    writeFileSync(file, DEFAULT_ACCOUNT_CONFIG_TOML, { encoding: 'utf8', mode: 0o600 })
  } catch {
    // 写不下不影响启动：codex 会用它自己的内置默认值。
  }
}

const DEFAULT_ACCOUNT_CONFIG_TOML = [
  '# Multi-AI Code 首次创建该账号目录时写入的默认值。',
  '# 改成 "unelevated" 或 "elevated" 可重新启用 Windows 沙箱；',
  '# elevated 需要 codex-windows-sandbox-setup.exe，当前安装包未携带。',
  '[windows]',
  'sandbox = "disabled"',
  ''
].join('\n')

/** The host owns CODEX_HOME; neither a caller nor an inherited shell may share it. */
export function withCodexAccountHome(
  env: Record<string, string>,
  accountHome: string
): Record<string, string> {
  if (!isAbsolute(accountHome)) throw new Error('Codex account home must be an absolute path')
  mkdirSync(accountHome, { recursive: true, mode: 0o700 })
  seedAccountCodexConfig(accountHome)
  const next = { ...env }
  // Windows environment variable names are case-insensitive. Remove aliases
  // before injecting the canonical key so the inherited value cannot win.
  for (const key of Object.keys(next)) {
    if (['CODEX_HOME', 'CODEX_SQLITE_HOME', 'CODEX_ANALYTICS_EVENTS_CAPTURE_FILE'].includes(key.toUpperCase())) delete next[key]
  }
  // Do not inherit a developer capture sink that writes outside the account.
  // Managed policy and CA settings are intentionally retained, not bypassed.
  next.CODEX_HOME = accountHome
  next.CODEX_SQLITE_HOME = accountHome
  return next
}

/** Only touch the effective account home, never ~/.codex or the target repo. */
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
