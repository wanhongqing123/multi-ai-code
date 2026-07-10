// codex 在 Windows ConPTY 下无法通过 OSC 10/11 探测宿主终端的背景色，会退化成"暗色"
// 假设，于是在我们这套亮色终端里选用一整套为暗背景设计的高亮色，导致浅色块、低对比文本
// （典型如状态栏、强调色几乎看不清）。fork 版 codex 新增了 CODEX_DEFAULT_TERMINAL_BG/FG
// 覆盖：把宿主终端真实的背景/前景色直接喂给 codex，让它的明暗判定（is_light）与我们实际
// 的终端主题一致，从根源取代早期"事后剥离背景 SGR"的脆弱做法（terminalCodexStyle）。
export const CODEX_DEFAULT_TERMINAL_BG_ENV = 'CODEX_DEFAULT_TERMINAL_BG'
export const CODEX_DEFAULT_TERMINAL_FG_ENV = 'CODEX_DEFAULT_TERMINAL_FG'

export type TerminalThemeMode = 'light' | 'dark'

// 与 src/components/mainTerminalConfig.ts 里 xterm 的 light/dark 主题保持一致；
// 值用不带 # 的 6 位十六进制，直接对应 codex fork 里 parse_hex_rgb 的解析格式。
const CODEX_TERMINAL_COLORS: Record<TerminalThemeMode, { bg: string; fg: string }> = {
  light: { bg: 'ffffff', fg: '000000' },
  dark: { bg: '1e1e1e', fg: 'e6e6e6' }
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

export function isCodexCommand(command: string): boolean {
  return /^codex(\.(exe|cmd|bat|ps1))?$/.test(basenameLike(command))
}

/**
 * 为 codex 会话注入宿主终端的默认背景/前景色。仅在命令是 codex 时生效；调用方已显式
 * 设置的同名 env 优先，不覆盖。theme 缺省按 light 处理（与终端主题的默认一致）。
 */
export function withCodexTerminalEnv(
  command: string,
  env: Record<string, string> | undefined,
  theme: TerminalThemeMode | undefined
): Record<string, string> | undefined {
  if (!isCodexCommand(command)) return env
  const next = { ...(env ?? {}) }
  const colors = CODEX_TERMINAL_COLORS[theme === 'dark' ? 'dark' : 'light']
  if (!next[CODEX_DEFAULT_TERMINAL_BG_ENV]) {
    next[CODEX_DEFAULT_TERMINAL_BG_ENV] = colors.bg
  }
  if (!next[CODEX_DEFAULT_TERMINAL_FG_ENV]) {
    next[CODEX_DEFAULT_TERMINAL_FG_ENV] = colors.fg
  }
  return next
}
