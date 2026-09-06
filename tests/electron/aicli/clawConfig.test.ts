import { describe, expect, it } from 'vitest'
import {
  CLAW_DEFAULT_BASE_URL,
  CLAW_DEFAULT_MODEL,
  EMPTY_CLAW_CONFIG,
  isClawCommand,
  withClawDataDirEnv,
  withClawManagedEnv,
  withClawModelArgs
} from '../../../electron/aicli/clawConfig.js'

const configured = { apiKey: 'zhipu-key', baseUrl: '', model: '' }

describe('isClawCommand', () => {
  it('matches the bare claw binary in any command form', () => {
    expect(isClawCommand('claw')).toBe(true)
    expect(isClawCommand('claw.exe')).toBe(true)
    expect(isClawCommand('"claw"')).toBe(true)
    expect(isClawCommand('C:\\Tools\\claw.exe')).toBe(true)
    expect(isClawCommand('/usr/local/bin/claw')).toBe(true)
  })

  it('does not match the other binaries in the claw workspace', () => {
    // claw-analog / claw-rag-service 是 claw workspace 里真实存在的二进制；
    // 判定少了 $ 锚点就会把它们也当成 claw，从而注入错误的 env 和 --model。
    expect(isClawCommand('claw-analog')).toBe(false)
    expect(isClawCommand('claw-rag-service')).toBe(false)
    expect(isClawCommand('clawx')).toBe(false)
    expect(isClawCommand('claude')).toBe(false)
  })
})

describe('withClawManagedEnv', () => {
  it('injects the managed key and the default endpoint', () => {
    expect(withClawManagedEnv('claw', undefined, configured)).toEqual({
      OPENAI_API_KEY: 'zhipu-key',
      OPENAI_BASE_URL: CLAW_DEFAULT_BASE_URL
    })
  })

  it('uses an explicitly configured base URL over the default', () => {
    expect(
      withClawManagedEnv('claw', undefined, { ...configured, baseUrl: 'https://api.x.ai/v1' })
    ).toEqual({
      OPENAI_API_KEY: 'zhipu-key',
      OPENAI_BASE_URL: 'https://api.x.ai/v1'
    })
  })

  // 这条是这个模块最重要的性质：设置里手写的 env 是用户指向别的 provider
  // （xAI / DashScope / Ollama / 自建）的唯一出口，托管配置绝不能盖掉它。
  it('never overrides env the user set explicitly', () => {
    const userEnv = {
      OPENAI_API_KEY: 'my-own-key',
      OPENAI_BASE_URL: 'http://127.0.0.1:11434/v1'
    }
    expect(withClawManagedEnv('claw', userEnv, configured)).toEqual(userEnv)
  })

  it('adds nothing when no key is configured', () => {
    expect(withClawManagedEnv('claw', undefined, EMPTY_CLAW_CONFIG)).toEqual({})
  })

  it('is an identity transform for other CLIs', () => {
    const env = { FOO: 'bar' }
    expect(withClawManagedEnv('codex', env, configured)).toBe(env)
    expect(withClawManagedEnv('claude', env, configured)).toBe(env)
    expect(withClawManagedEnv('claw-analog', env, configured)).toBe(env)
  })
})

describe('withClawModelArgs', () => {
  it('prepends the default model so claw does not fall back to Anthropic', () => {
    // claw 缺省是 anthropic/claude-*，在智谱托管配置下必然认证失败。
    expect(withClawModelArgs('claw', [], configured)).toEqual(['--model', CLAW_DEFAULT_MODEL])
  })

  it('uses an explicitly configured model', () => {
    expect(withClawModelArgs('claw', [], { ...configured, model: 'openai/glm-4.5-air' })).toEqual([
      '--model',
      'openai/glm-4.5-air'
    ])
  })

  it('leaves an explicit --model in the args untouched, in both forms', () => {
    expect(withClawModelArgs('claw', ['--model', 'anthropic/claude-opus-4-7'], configured)).toEqual([
      '--model',
      'anthropic/claude-opus-4-7'
    ])
    expect(withClawModelArgs('claw', ['--model=xai/grok-4'], configured)).toEqual([
      '--model=xai/grok-4'
    ])
  })

  it('adds nothing when no key is configured', () => {
    expect(withClawModelArgs('claw', ['--verbose'], EMPTY_CLAW_CONFIG)).toEqual(['--verbose'])
  })

  it('is an identity transform for other CLIs', () => {
    expect(withClawModelArgs('codex', ['resume'], configured)).toEqual(['resume'])
    expect(withClawModelArgs('claw-analog', ['doctor'], configured)).toEqual(['doctor'])
  })
})

describe('withClawDataDirEnv', () => {
  it('moves claw session storage out of the user repo', () => {
    expect(withClawDataDirEnv('claw', undefined, 'C:/data/aicli/claw')).toEqual({
      CLAW_DATA_DIR: 'C:/data/aicli/claw'
    })
  })

  // 与凭据分开注入是刻意的：仓库污染跟有没有配 provider 无关，
  // 没配 Key 也必须生效。合进 withClawManagedEnv 会让它在无 Key 时静默失效。
  it('applies even when no API key is configured', () => {
    const withKey = withClawManagedEnv('claw', undefined, EMPTY_CLAW_CONFIG)
    expect(withKey).toEqual({})
    expect(withClawDataDirEnv('claw', withKey, 'C:/data/aicli/claw')).toEqual({
      CLAW_DATA_DIR: 'C:/data/aicli/claw'
    })
  })

  it('never overrides an explicitly configured data dir', () => {
    const userEnv = { CLAW_DATA_DIR: 'D:/my/own/place' }
    expect(withClawDataDirEnv('claw', userEnv, 'C:/data/aicli/claw')).toEqual(userEnv)
  })

  it('is an identity transform for other CLIs', () => {
    const env = { FOO: 'bar' }
    expect(withClawDataDirEnv('codex', env, 'C:/data')).toBe(env)
    expect(withClawDataDirEnv('claw-analog', env, 'C:/data')).toBe(env)
  })
})
