import { describe, expect, it } from 'vitest'
import {
  isOpenCodeCommand,
  OPENCODE_LSP_CONFIG_CONTENT,
  withOpenCodeManagedProfileEnv,
  withOpenCodeLspEnv
} from '../../../electron/aicli/opencodeConfig.js'

describe('OpenCode config env', () => {
  it('recognizes bare and path-based OpenCode commands', () => {
    expect(isOpenCodeCommand('opencode')).toBe(true)
    expect(isOpenCodeCommand('/custom/bin/opencode')).toBe(true)
    expect(isOpenCodeCommand('"C:\\Tools\\opencode.exe"')).toBe(true)
    expect(isOpenCodeCommand('codex')).toBe(false)
    expect(isOpenCodeCommand('my-opencode-wrapper')).toBe(false)
  })

  it('enables LSP for OpenCode without touching other env values', () => {
    const env = withOpenCodeLspEnv('opencode', { FOO: 'bar' })

    expect(env).toMatchObject({
      FOO: 'bar',
      OPENCODE_CONFIG_CONTENT: OPENCODE_LSP_CONFIG_CONTENT
    })
    expect(JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')).toEqual({
      $schema: 'https://opencode.ai/config.json',
      lsp: true,
      autoupdate: false
    })
  })

  it('follows the host app theme for the OpenCode TUI mode', () => {
    // 缺省（未传 theme）回退 light，与旧行为一致。
    expect(withOpenCodeLspEnv('opencode', { FOO: 'bar' })?.OPENCODE_THEME_MODE).toBe('light')
    // 暗色 app → 暗色 TUI。
    expect(
      withOpenCodeLspEnv('opencode', { FOO: 'bar' }, undefined, 'dark')?.OPENCODE_THEME_MODE
    ).toBe('dark')
    expect(
      withOpenCodeLspEnv('opencode', { FOO: 'bar' }, undefined, 'light')?.OPENCODE_THEME_MODE
    ).toBe('light')
  })

  it('disables the upgrade prompt via env flag (config autoupdate does not reach getGlobal)', () => {
    const env = withOpenCodeLspEnv('opencode', { FOO: 'bar' })
    expect(env?.OPENCODE_DISABLE_AUTOUPDATE).toBe('1')
    // 用户显式设置优先（包括显式关闭 flag 的 '0'）。
    const custom = withOpenCodeLspEnv('opencode', { OPENCODE_DISABLE_AUTOUPDATE: '0' })
    expect(custom?.OPENCODE_DISABLE_AUTOUPDATE).toBe('0')
  })

  it('respects a user-provided OPENCODE_THEME_MODE', () => {
    const env = withOpenCodeLspEnv('opencode', { OPENCODE_THEME_MODE: 'dark' })
    expect(env?.OPENCODE_THEME_MODE).toBe('dark')
  })

  it('does not inject OpenCode config for Claude or Codex', () => {
    expect(withOpenCodeLspEnv('claude', { FOO: 'bar' })).toEqual({ FOO: 'bar' })
    expect(withOpenCodeLspEnv('codex', { FOO: 'bar' })).toEqual({ FOO: 'bar' })
  })

  it('preserves explicit user lsp/autoupdate values while filling in defaults', () => {
    const content = JSON.stringify({ model: 'zhipu/glm-4', lsp: false })
    const env = withOpenCodeLspEnv('opencode', {
      OPENCODE_CONFIG_CONTENT: content
    })

    expect(JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')).toEqual({
      $schema: 'https://opencode.ai/config.json',
      model: 'zhipu/glm-4',
      lsp: false,
      autoupdate: false
    })
  })

  it('keeps config content untouched when lsp and autoupdate are both explicit', () => {
    const content = JSON.stringify({ lsp: false, autoupdate: 'notify' })
    const env = withOpenCodeLspEnv('opencode', {
      OPENCODE_CONFIG_CONTENT: content
    })

    expect(env?.OPENCODE_CONFIG_CONTENT).toBe(content)
  })

  it('merges LSP into existing config content when it is omitted', () => {
    const env = withOpenCodeLspEnv('opencode', {
      OPENCODE_CONFIG_CONTENT: JSON.stringify({ model: 'zhipu/glm-4' })
    })

    expect(JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')).toEqual({
      $schema: 'https://opencode.ai/config.json',
      model: 'zhipu/glm-4',
      lsp: true,
      autoupdate: false
    })
  })

  it('keeps invalid config content unchanged instead of hiding the user error', () => {
    const env = withOpenCodeLspEnv('opencode', {
      OPENCODE_CONFIG_CONTENT: '{bad json'
    })

    expect(env?.OPENCODE_CONFIG_CONTENT).toBe('{bad json')
  })

  it('injects the managed profile and overrides legacy provider selection', () => {
    const env = withOpenCodeManagedProfileEnv(
      {
        OPENCODE_CONFIG_CONTENT: JSON.stringify({
          model: 'legacy/old',
          disabled_providers: ['zhipu'],
          provider: { legacy: { models: { old: { name: 'old' } } } },
          share: 'disabled'
        })
      },
      {
        version: 1,
        defaultModel: 'zhipu/glm-5.3',
        smallModel: 'zhipu/glm-5.3',
        enabledProviders: ['zhipu']
      },
      { ZHIPU_API_KEY: 'managed-zhipu' }
    )
    const config = JSON.parse(env.OPENCODE_CONFIG_CONTENT)

    expect(env.ZHIPU_API_KEY).toBe('managed-zhipu')
    expect(config).toMatchObject({
      model: 'zhipu/glm-5.3',
      small_model: 'zhipu/glm-5.3',
      enabled_providers: ['zhipu'],
      share: 'disabled'
    })
    expect(config).not.toHaveProperty('provider')
    expect(config).not.toHaveProperty('disabled_providers')
  })
})
