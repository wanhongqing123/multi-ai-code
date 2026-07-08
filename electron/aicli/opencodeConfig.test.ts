import { describe, expect, it } from 'vitest'
import {
  isOpenCodeCommand,
  OPENCODE_LSP_CONFIG_CONTENT,
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
      lsp: true
    })
  })

  it('does not inject OpenCode config for Claude or Codex', () => {
    expect(withOpenCodeLspEnv('claude', { FOO: 'bar' })).toEqual({ FOO: 'bar' })
    expect(withOpenCodeLspEnv('codex', { FOO: 'bar' })).toEqual({ FOO: 'bar' })
  })

  it('preserves an explicit user LSP setting', () => {
    const content = JSON.stringify({ model: 'zhipu/glm-4', lsp: false })
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
      lsp: true
    })
  })

  it('keeps invalid config content unchanged instead of hiding the user error', () => {
    const env = withOpenCodeLspEnv('opencode', {
      OPENCODE_CONFIG_CONTENT: '{bad json'
    })

    expect(env?.OPENCODE_CONFIG_CONTENT).toBe('{bad json')
  })
})
