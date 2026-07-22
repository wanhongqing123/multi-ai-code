import { afterEach, describe, expect, it } from 'vitest'
import {
  buildMainTerminalOptions,
  shouldConvertEolForCli,
  xtermThemeFor
} from './mainTerminalConfig.js'

const originalPlatform =
  typeof navigator !== 'undefined' ? navigator.platform : undefined

function mockNavigatorPlatform(platform: string): void {
  if (typeof navigator === 'undefined') {
    Object.defineProperty(globalThis, 'navigator', {
      value: { platform },
      configurable: true
    })
    return
  }

  Object.defineProperty(navigator, 'platform', {
    value: platform,
    configurable: true
  })
}

afterEach(() => {
  if (originalPlatform === undefined) {
    Reflect.deleteProperty(globalThis, 'navigator')
    return
  }

  Object.defineProperty(navigator, 'platform', {
    value: originalPlatform,
    configurable: true
  })
})

describe('shouldConvertEolForCli', () => {
  it('disables convertEol for opencode so bare LF keeps VT index semantics', () => {
    expect(shouldConvertEolForCli('opencode')).toBe(false)
    expect(shouldConvertEolForCli('opencode.exe')).toBe(false)
    expect(shouldConvertEolForCli('C:\\Tools\\opencode.exe')).toBe(false)
    expect(shouldConvertEolForCli('/usr/local/bin/opencode')).toBe(false)
  })

  it('keeps convertEol on for claude, codex and unknown CLIs', () => {
    expect(shouldConvertEolForCli('claude')).toBe(true)
    expect(shouldConvertEolForCli('codex')).toBe(true)
    expect(shouldConvertEolForCli(undefined)).toBe(true)
    expect(shouldConvertEolForCli('my-opencode-wrapper')).toBe(true)
  })
})

describe('xtermThemeFor', () => {
  it('returns the light palette for light theme', () => {
    expect(xtermThemeFor('light')).toMatchObject({
      background: '#FFFFFF',
      foreground: '#000000'
    })
  })

  it('returns the dark palette for dark theme', () => {
    expect(xtermThemeFor('dark')).toMatchObject({
      background: '#1e1e1e',
      foreground: '#e6e6e6'
    })
  })
})

describe('buildMainTerminalOptions', () => {
  it('disables smooth scrolling for large-output sessions', () => {
    expect(buildMainTerminalOptions('light')).toMatchObject({
      smoothScrollDuration: 0
    })
  })

  it('keeps convertEol for claude but turns it off for opencode', () => {
    expect(buildMainTerminalOptions('light', 'claude').convertEol).toBe(true)
    expect(buildMainTerminalOptions('light').convertEol).toBe(true)
    expect(buildMainTerminalOptions('light', 'opencode').convertEol).toBe(false)
  })

  it('keeps a large scrollback for long AICLI PTY transcripts', () => {
    expect(buildMainTerminalOptions('light').scrollback).toBeGreaterThanOrEqual(50_000)
  })

  it('uses heavier weights and larger size on Windows; lighter elsewhere', () => {
    const opts = buildMainTerminalOptions('light')
    const plat =
      typeof navigator !== 'undefined' ? navigator.platform.toLowerCase() : ''
    if (plat.includes('win')) {
      expect(opts).toMatchObject({
        fontWeight: 600,
        fontWeightBold: 800,
        fontSize: 13
      })
    } else {
      expect(opts).toMatchObject({
        fontWeight: 400,
        fontWeightBold: 700,
        fontSize: 12
      })
    }
  })

  it('keeps the programmer-friendly mono stack', () => {
    const fontFamily = String(buildMainTerminalOptions('light').fontFamily)
    expect(fontFamily).toContain('SF Mono')
    expect(fontFamily).toContain('Menlo')
    expect(fontFamily).toContain('JetBrains Mono')
  })

  it('uses a native-terminal-like render profile without extra spacing', () => {
    expect(buildMainTerminalOptions('light')).toMatchObject({
      lineHeight: 1.15,
      letterSpacing: 0,
      minimumContrastRatio: 4.5,
      cursorBlink: false
    })
  })

  it('uses a tighter Windows profile only for codex and opencode', () => {
    mockNavigatorPlatform('Win32')

    expect(buildMainTerminalOptions('light', 'codex')).toMatchObject({
      lineHeight: 1.25,
      fontWeight: 500,
      fontWeightBold: 700
    })
    expect(buildMainTerminalOptions('light', 'C:\\Tools\\opencode.exe')).toMatchObject({
      lineHeight: 1.25,
      fontWeight: 500,
      fontWeightBold: 700
    })
    expect(buildMainTerminalOptions('light', 'claude')).toMatchObject({
      lineHeight: 1.45,
      fontWeight: 600,
      fontWeightBold: 800
    })
  })
})
