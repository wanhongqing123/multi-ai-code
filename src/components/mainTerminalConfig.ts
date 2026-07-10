import type { ITerminalOptions } from '@xterm/xterm'
import type { Theme } from '../utils/theme.js'

const XTERM_DARK_THEME = {
  background: '#1e1e1e',
  foreground: '#e6e6e6'
}

const XTERM_LIGHT_THEME = {
  background: '#FFFFFF',
  foreground: '#000000',
  cursor: '#202124',
  cursorAccent: '#FFFFFF',
  selectionBackground: 'rgba(26, 115, 232, 0.2)',
  black: '#202124',
  red: '#D93025',
  green: '#1E8E3E',
  yellow: '#B06000',
  blue: '#1A73E8',
  magenta: '#9334E6',
  cyan: '#0086A3',
  white: '#5F6368',
  brightBlack: '#5F6368',
  brightRed: '#D93025',
  brightGreen: '#1E8E3E',
  brightYellow: '#B06000',
  brightBlue: '#1A73E8',
  brightMagenta: '#9334E6',
  brightCyan: '#0086A3',
  brightWhite: '#202124'
}

export const MAIN_TERMINAL_SCROLLBACK_LINES = 50_000

export function xtermThemeFor(
  theme: Theme
): typeof XTERM_DARK_THEME | typeof XTERM_LIGHT_THEME {
  return theme === 'dark' ? XTERM_DARK_THEME : XTERM_LIGHT_THEME
}

function isWindowsPlatform(): boolean {
  const plat =
    typeof navigator !== 'undefined' ? navigator.platform.toLowerCase() : ''
  return plat.includes('win')
}

function isCliExecutable(cli: string | undefined, names: readonly string[]): boolean {
  if (!cli) return false
  const escapedNames = names.map((name) => name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
  return new RegExp(
    `(^|[\\\\/])(${escapedNames.join('|')})(\\.(exe|cmd|bat|ps1))?$`,
    'i'
  ).test(cli.trim())
}

/**
 * opencode 的 opentui 渲染器用裸 LF 表示"下移一行、列不变"（VT 标准语义，
 * 实测字节流中侧栏刷新即如此）。convertEol 会把它改写成 CR+LF，光标被拽回
 * 第 0 列后，后续相对定位全部错位，屏幕上留下残影碎片，因此必须关闭。
 * claude/codex 维持原有 convertEol=true 行为不变。
 */
export function shouldConvertEolForCli(cli: string | undefined): boolean {
  return !isCliExecutable(cli, ['opencode'])
}

export function buildMainTerminalOptions(theme: Theme, cli?: string): ITerminalOptions {
  // Windows 下 GDI/DirectWrite 渲染较瘦，保留较粗权重保证清晰度；
  // macOS / Linux 对齐系统终端的紧凑行距和常用字体，避免 TUI 被撑高、显得松散。
  const heavy = isWindowsPlatform()
  const tunedWindowsAicli = heavy && isCliExecutable(cli, ['codex', 'opencode'])
  return {
    fontSize: heavy ? 13 : 12,
    lineHeight: tunedWindowsAicli ? 1.25 : heavy ? 1.45 : 1.15,
    letterSpacing: 0,
    fontFamily:
      tunedWindowsAicli
        ? '"Cascadia Mono", Consolas, "JetBrains Mono", monospace'
        : '"SF Mono", Menlo, Monaco, "JetBrains Mono", Consolas, monospace',
    fontWeight: tunedWindowsAicli ? 500 : heavy ? 600 : 400,
    fontWeightBold: tunedWindowsAicli ? 700 : heavy ? 800 : 700,
    cursorBlink: false,
    cursorStyle: 'underline',
    cursorInactiveStyle: 'underline',
    cursorWidth: 1,
    convertEol: shouldConvertEolForCli(cli),
    // Force xterm to auto-lighten/darken the foreground when a CLI-emitted
    // background (highlight blocks, selection bars, etc.) would otherwise
    // swallow the text. 4.5 is the WCAG AA threshold for body text.
    minimumContrastRatio: 4.5,
    scrollback: MAIN_TERMINAL_SCROLLBACK_LINES,
    smoothScrollDuration: 0,
    theme: xtermThemeFor(theme),
    allowProposedApi: true
  }
}

export function shouldEnableMainTerminalGpuAcceleration(): boolean {
  return false
}

export function shouldUseMainTerminalCanvasRenderer(): boolean {
  return true
}
