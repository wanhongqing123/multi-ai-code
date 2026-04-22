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

export function buildMainTerminalOptions(theme: Theme): ITerminalOptions {
  return {
    fontSize: 13,
    lineHeight: 1.45,
    letterSpacing: 0,
    fontFamily:
      'Monaco, Menlo, "JetBrains Mono", "SF Mono", Consolas, monospace',
    fontWeight: 600,
    fontWeightBold: 800,
    cursorBlink: false,
    cursorStyle: 'underline',
    cursorInactiveStyle: 'underline',
    cursorWidth: 1,
    convertEol: true,
    minimumContrastRatio: 1,
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
