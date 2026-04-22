import { describe, expect, it } from 'vitest'
import {
  buildMainTerminalOptions,
  shouldUseMainTerminalCanvasRenderer,
  shouldEnableMainTerminalGpuAcceleration,
  xtermThemeFor
} from './mainTerminalConfig.js'

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
      smoothScrollDuration: 0,
      fontWeight: 600,
      fontWeightBold: 800
    })
  })

  it('keeps the programmer-friendly mono stack', () => {
    expect(buildMainTerminalOptions('light').fontFamily).toContain(
      'JetBrains Mono'
    )
  })

  it('uses the leaner render profile for long transcript scrolling', () => {
    expect(buildMainTerminalOptions('light')).toMatchObject({
      lineHeight: 1.45,
      letterSpacing: 0,
      minimumContrastRatio: 1,
      cursorBlink: false
    })
  })
})

describe('shouldEnableMainTerminalGpuAcceleration', () => {
  it('stays disabled for stability in Electron renderer sessions', () => {
    expect(shouldEnableMainTerminalGpuAcceleration()).toBe(false)
  })
})

describe('shouldUseMainTerminalCanvasRenderer', () => {
  it('prefers canvas over the default DOM renderer for long transcript scrolling', () => {
    expect(shouldUseMainTerminalCanvasRenderer()).toBe(true)
  })
})
