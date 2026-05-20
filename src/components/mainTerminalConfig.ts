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

export function buildMainTerminalOptions(theme: Theme): ITerminalOptions {
  // Windows 下 GDI/DirectWrite 渲染较瘦，保留较粗权重保证清晰度；
  // macOS / Linux 的字体平滑会让相同权重显得过粗，降到 normal/bold。
  const heavy = isWindowsPlatform()
  return {
    fontSize: heavy ? 13 : 11,
    lineHeight: 1.45,
    letterSpacing: 0,
    fontFamily:
      'Monaco, Menlo, "JetBrains Mono", "SF Mono", Consolas, monospace',
    fontWeight: heavy ? 600 : 400,
    fontWeightBold: heavy ? 800 : 700,
    cursorBlink: false,
    cursorStyle: 'underline',
    cursorInactiveStyle: 'underline',
    cursorWidth: 1,
    convertEol: true,
    // Force xterm to auto-lighten/darken the foreground when a CLI-emitted
    // background (highlight blocks, selection bars, etc.) would otherwise
    // swallow the text. 4.5 is the WCAG AA threshold for body text.
    minimumContrastRatio: 4.5,
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
