import { describe, expect, it } from 'vitest'
import {
  isOpenCodeCommand,
  OPENCODE_LSP_CONFIG_CONTENT,
  type OpenCodeProviderProfile,
  withOpenCodeLspEnv
} from './opencodeConfig.js'

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

  it('pins the OpenCode TUI theme mode to light for a unified look', () => {
    const env = withOpenCodeLspEnv('opencode', { FOO: 'bar' })
    expect(env?.OPENCODE_THEME_MODE).toBe('light')
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

  it('injects a custom OpenCode provider profile with baseURL and selected models', () => {
    const profile: OpenCodeProviderProfile = {
      providerId: 'multi-ai-deepseek-internal',
      name: '公司内网 DeepSeek',
      baseURL: 'https://llm.example.test/v1',
      apiKey: 'test-api-key',
      mainModel: 'deepseek-v4-pro',
      smallModel: 'deepseek-v4-lite',
      timeoutMs: 600000,
      chunkTimeoutMs: 60000
    }

    const env = withOpenCodeLspEnv('opencode', { FOO: 'bar' }, profile)
    const config = JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')

    expect(config).toMatchObject({
      $schema: 'https://opencode.ai/config.json',
      lsp: true,
      autoupdate: false,
      model: 'multi-ai-deepseek-internal/deepseek-v4-pro',
      small_model: 'multi-ai-deepseek-internal/deepseek-v4-lite',
      provider: {
        'multi-ai-deepseek-internal': {
          npm: '@ai-sdk/openai-compatible',
          name: '公司内网 DeepSeek',
          options: {
            baseURL: 'https://llm.example.test/v1',
            apiKey: 'test-api-key',
            timeout: 600000,
            chunkTimeout: 60000
          },
          models: {
            'deepseek-v4-pro': {
              name: 'deepseek-v4-pro'
            },
            'deepseek-v4-lite': {
              name: 'deepseek-v4-lite'
            }
          }
        }
      }
    })
  })

  it('merges a custom provider profile into existing inline OpenCode config', () => {
    const env = withOpenCodeLspEnv(
      'opencode',
      {
        OPENCODE_CONFIG_CONTENT: JSON.stringify({
          provider: {
            existing: {
              models: {
                old: { name: 'old' }
              }
            }
          },
          share: 'disabled'
        })
      },
      {
        providerId: 'multi-ai-zhipu',
        name: '智谱 AI',
        baseURL: 'https://open.bigmodel.cn/api/coding/paas/v4',
        apiKey: 'zai-api-key',
        mainModel: 'glm-5.2'
      }
    )
    const config = JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')

    expect(config.provider.existing.models.old.name).toBe('old')
    expect(config.provider['multi-ai-zhipu'].options.baseURL).toBe(
      'https://open.bigmodel.cn/api/coding/paas/v4'
    )
    expect(config.model).toBe('multi-ai-zhipu/glm-5.2')
    expect(config.small_model).toBe('multi-ai-zhipu/glm-5.2')
    expect(config.share).toBe('disabled')
  })
})
