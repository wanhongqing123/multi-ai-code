import { describe, expect, it } from 'vitest'
import {
  CODEX_DEFAULT_TERMINAL_BG_ENV,
  CODEX_DEFAULT_TERMINAL_FG_ENV,
  isCodexCommand,
  withCodexTerminalEnv
} from './codexConfig.js'

describe('Codex terminal env', () => {
  it('recognizes bare and path-based codex commands only', () => {
    expect(isCodexCommand('codex')).toBe(true)
    expect(isCodexCommand('/usr/local/bin/codex')).toBe(true)
    expect(isCodexCommand('"C:\\Tools\\codex.exe"')).toBe(true)
    expect(isCodexCommand('opencode')).toBe(false)
    expect(isCodexCommand('claude')).toBe(false)
    expect(isCodexCommand('my-codex-wrapper')).toBe(false)
  })

  it('injects light terminal colors for a light host theme', () => {
    const env = withCodexTerminalEnv('codex', { FOO: 'bar' }, 'light')
    expect(env).toMatchObject({
      FOO: 'bar',
      [CODEX_DEFAULT_TERMINAL_BG_ENV]: 'ffffff',
      [CODEX_DEFAULT_TERMINAL_FG_ENV]: '000000'
    })
  })

  it('injects dark terminal colors for a dark host theme', () => {
    const env = withCodexTerminalEnv('codex', undefined, 'dark')
    expect(env).toMatchObject({
      [CODEX_DEFAULT_TERMINAL_BG_ENV]: '1e1e1e',
      [CODEX_DEFAULT_TERMINAL_FG_ENV]: 'e6e6e6'
    })
  })

  it('defaults to light colors when theme is undefined', () => {
    const env = withCodexTerminalEnv('codex', undefined, undefined)
    expect(env?.[CODEX_DEFAULT_TERMINAL_BG_ENV]).toBe('ffffff')
    expect(env?.[CODEX_DEFAULT_TERMINAL_FG_ENV]).toBe('000000')
  })

  it('does not override caller-provided terminal colors', () => {
    const env = withCodexTerminalEnv(
      'codex',
      {
        [CODEX_DEFAULT_TERMINAL_BG_ENV]: '123456',
        [CODEX_DEFAULT_TERMINAL_FG_ENV]: 'abcdef'
      },
      'light'
    )
    expect(env?.[CODEX_DEFAULT_TERMINAL_BG_ENV]).toBe('123456')
    expect(env?.[CODEX_DEFAULT_TERMINAL_FG_ENV]).toBe('abcdef')
  })

  it('leaves env untouched for non-codex commands', () => {
    const input = { FOO: 'bar' }
    const env = withCodexTerminalEnv('opencode', input, 'light')
    expect(env).toBe(input)
    expect(env?.[CODEX_DEFAULT_TERMINAL_BG_ENV]).toBeUndefined()
  })
})
