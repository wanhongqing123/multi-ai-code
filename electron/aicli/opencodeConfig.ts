export const OPENCODE_LSP_CONFIG_CONTENT = JSON.stringify({
  $schema: 'https://opencode.ai/config.json',
  lsp: true,
  // 自编译版本号是 0.0.0-dev-*，永远小于官方 release，不关掉会每次启动都弹升级提示。
  autoupdate: false
})

// opencode fork 会把 OPENCODE_THEME_MODE 当成明暗模式的锁定值（见 fork 的
// packages/tui/src/context/theme.tsx），不跟随宿主终端背景探测（ConPTY 下探测不可靠）。
// 这里按宿主 app 的明暗主题传入，让 TUI 与我们的终端主题一致；未知主题回退 light。
export const OPENCODE_THEME_MODE_ENV = 'OPENCODE_THEME_MODE'
export const OPENCODE_THEME_MODE_DEFAULT = 'light'

export type OpenCodeThemeMode = 'light' | 'dark'

// 升级检查读的是全局配置文件（Config.getGlobal），OPENCODE_CONFIG_CONTENT 里的
// autoupdate:false 覆盖不到它；这个 env flag 是进程级开关，能彻底关掉升级弹窗。
export const OPENCODE_DISABLE_AUTOUPDATE_ENV = 'OPENCODE_DISABLE_AUTOUPDATE'

export interface OpenCodeProviderProfile {
  providerId?: string
  name?: string
  baseURL?: string
  apiKey?: string
  mainModel?: string
  smallModel?: string
  timeoutMs?: number
  chunkTimeoutMs?: number
}

export interface OpenCodeManagedProfile {
  version: 1
  defaultModel: string
  smallModel: string
  enabledProviders: string[]
}

function basenameLike(command: string): string {
  let normalized = command.trim()
  while (normalized.length >= 2) {
    const first = normalized[0]
    const last = normalized[normalized.length - 1]
    if ((first === '"' && last === '"') || (first === "'" && last === "'")) {
      normalized = normalized.slice(1, -1).trim()
      continue
    }
    break
  }
  const parts = normalized.split(/[\\/]+/)
  return (parts[parts.length - 1] ?? normalized).toLowerCase()
}

export function isOpenCodeCommand(command: string): boolean {
  return /^opencode(\.(exe|cmd|bat|ps1))?$/.test(basenameLike(command))
}

function parseOpenCodeConfigContent(content: string): Record<string, unknown> | null {
  let parsed: unknown
  try {
    parsed = JSON.parse(content)
  } catch {
    return null
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return null
  return parsed as Record<string, unknown>
}

function mergeOpenCodeConfigContent(content: string): string | null {
  const parsed = parseOpenCodeConfigContent(content)
  if (!parsed) return null

  const hasLsp = Object.prototype.hasOwnProperty.call(parsed, 'lsp')
  const hasAutoupdate = Object.prototype.hasOwnProperty.call(parsed, 'autoupdate')
  if (hasLsp && hasAutoupdate) return content

  const next: Record<string, unknown> = {
    $schema: 'https://opencode.ai/config.json',
    ...parsed
  }
  if (!hasLsp) next.lsp = true
  // 始终默认关闭自动升级（自编译版本 0.0.0 会每次触发升级提示），用户显式配置优先。
  if (!hasAutoupdate) next.autoupdate = false

  return JSON.stringify(next)
}

export function withOpenCodeManagedProfileEnv(
  env: Record<string, string> | undefined,
  profile: OpenCodeManagedProfile,
  credentialEnv: Record<string, string>
): Record<string, string> {
  const next = { ...(env ?? {}), ...credentialEnv }

  const parsed = parseOpenCodeConfigContent(next.OPENCODE_CONFIG_CONTENT ?? '') ??
    parseOpenCodeConfigContent(OPENCODE_LSP_CONFIG_CONTENT)!
  const config: Record<string, unknown> = {
    $schema: 'https://opencode.ai/config.json',
    ...parsed,
    model: profile.defaultModel,
    small_model: profile.smallModel,
    enabled_providers: [...profile.enabledProviders]
  }
  // Provider definitions and disabled lists from legacy project settings must
  // not expand or override the reviewed managed catalog.
  delete config.provider
  delete config.disabled_providers
  next.OPENCODE_CONFIG_CONTENT = JSON.stringify(config)
  return next
}

export function withOpenCodeLspEnv(
  command: string,
  env: Record<string, string> | undefined,
  _profile?: OpenCodeProviderProfile,
  theme?: OpenCodeThemeMode
): Record<string, string> | undefined {
  if (!isOpenCodeCommand(command)) return env
  const next = { ...(env ?? {}) }
  // 用户显式设置的 OPENCODE_THEME_MODE 优先；否则跟随宿主 app 主题（缺省 light）。
  if (!next[OPENCODE_THEME_MODE_ENV]) {
    next[OPENCODE_THEME_MODE_ENV] = theme === 'dark' ? 'dark' : OPENCODE_THEME_MODE_DEFAULT
  }
  if (!next[OPENCODE_DISABLE_AUTOUPDATE_ENV]) {
    next[OPENCODE_DISABLE_AUTOUPDATE_ENV] = '1'
  }
  const existing = next.OPENCODE_CONFIG_CONTENT
  if (!existing) {
    next.OPENCODE_CONFIG_CONTENT = mergeOpenCodeConfigContent(OPENCODE_LSP_CONFIG_CONTENT) ??
      OPENCODE_LSP_CONFIG_CONTENT
    return next
  }
  const merged = mergeOpenCodeConfigContent(existing)
  if (merged) next.OPENCODE_CONFIG_CONTENT = merged
  return next
}
