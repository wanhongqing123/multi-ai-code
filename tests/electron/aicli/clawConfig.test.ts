import { describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import {
  CLAW_ANTHROPIC_BASE_URL,
  CLAW_DEFAULT_BASE_URL,
  CLAW_DEFAULT_MODEL,
  clawCanonicalModel,
  EMPTY_CLAW_CONFIG,
  isClawCommand,
  withClawDataDirEnv,
  withClawManagedEnv,
  withClawModelArgs
} from '../../../electron/aicli/clawConfig.js'

const configured = { apiKey: 'zhipu-key', baseUrl: '', model: '' }

// claw 的默认端点/模型必须与 opencode 那份托管目录一致——两者用的是同一把智谱 Key。
// 这组用例是因为真出过事才加的：默认 base URL 少写了 `/coding/` 那一段、
// 默认模型 glm-4.6 根本不在目录里，用户一跑就是
//     {"code":500,"msg":"404 NOT_FOUND","success":false}
// 当时用 mock 端点验证过「机制可用」，但 mock 任何路径任何模型名都接受，
// 证明不了真实端点对不对。所以改成直接对着仓库里的目录钉死。
describe('claw defaults track the managed Zhipu catalog', () => {
  const catalog = JSON.parse(
    readFileSync(join(process.cwd(), 'resources', 'opencode', 'managed-models.json'), 'utf8')
  ) as { zhipu: { api: string; models: Record<string, unknown> } }

  it('uses the same endpoint opencode uses', () => {
    expect(CLAW_DEFAULT_BASE_URL).toBe(catalog.zhipu.api)
  })

  it('defaults to a model that actually exists in the catalog', () => {
    // claw 按前缀路由，线上发出去的是去掉 openai/ 之后的名字。
    const wireModel = CLAW_DEFAULT_MODEL.replace(/^openai\//, '')
    expect(Object.keys(catalog.zhipu.models)).toContain(wireModel)
  })

  it('keeps the provider prefix, or claw would route to Anthropic', () => {
    expect(CLAW_DEFAULT_MODEL.startsWith('openai/')).toBe(true)
  })
})

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

// 这一组钉的是一次真实事故：用户把 Base URL 填成智谱的 Anthropic 端点、模型填裸名
// `glm-5.3`，结果 claw 报 404 NOT_FOUND。原因是代码无论模型走哪个 provider，
// 都把 Base URL 写进 OPENAI_BASE_URL，于是请求打到
//     https://open.bigmodel.cn/api/anthropic/chat/completions
// 这个不存在的路径上。一个输入框对应两组互不相干的 env，必须按前缀分派。
describe('provider is chosen by the model prefix, and env follows it', () => {
  const key = { apiKey: 'zhipu-key' }

  it('routes an anthropic/ model to the Anthropic env pair', () => {
    const env = withClawManagedEnv('claw', undefined, {
      ...key,
      baseUrl: 'https://open.bigmodel.cn/api/anthropic',
      model: 'anthropic/glm-5.3'
    })
    expect(env).toEqual({
      ANTHROPIC_API_KEY: 'zhipu-key',
      ANTHROPIC_BASE_URL: 'https://open.bigmodel.cn/api/anthropic'
    })
    // 事故的核心：绝不能把 Anthropic 端点写进 OPENAI_BASE_URL
    expect(env).not.toHaveProperty('OPENAI_BASE_URL')
  })

  it('routes an openai/ model to the OpenAI env pair', () => {
    expect(
      withClawManagedEnv('claw', undefined, { ...key, baseUrl: '', model: 'openai/glm-5.3' })
    ).toEqual({
      OPENAI_API_KEY: 'zhipu-key',
      OPENAI_BASE_URL: CLAW_DEFAULT_BASE_URL
    })
  })

  it('falls back to the matching default endpoint per provider', () => {
    const env = withClawManagedEnv('claw', undefined, {
      ...key,
      baseUrl: '',
      model: 'anthropic/glm-5.3'
    })
    expect(env?.ANTHROPIC_BASE_URL).toBe(CLAW_ANTHROPIC_BASE_URL)
  })

  // 裸名在 claw 里不走前缀路由，而是落到「按已设置的鉴权变量嗅探」，
  // 结果取决于我们注入了什么，用户无法预期。所以要补成确定的形式。
  it('normalizes a bare model name instead of leaving routing to the auth sniffer', () => {
    expect(clawCanonicalModel('glm-5.3')).toBe('openai/glm-5.3')
    expect(clawCanonicalModel('')).toBe(CLAW_DEFAULT_MODEL)
    // 已带前缀的不动
    expect(clawCanonicalModel('anthropic/glm-5.3')).toBe('anthropic/glm-5.3')
    // claw 自己认得的裸名不要画蛇添足
    expect(clawCanonicalModel('gpt-4.1-mini')).toBe('gpt-4.1-mini')
    expect(clawCanonicalModel('grok-4')).toBe('grok-4')
  })

  // env 与 args 必须基于同一个模型名判定，否则填裸名时两者会指向不同 provider。
  it('keeps env and --model consistent for a bare model name', () => {
    const config = { ...key, baseUrl: '', model: 'glm-5.3' }
    const args = withClawModelArgs('claw', [], config)
    const env = withClawManagedEnv('claw', undefined, config)
    expect(args).toEqual(['--model', 'openai/glm-5.3'])
    expect(env).toHaveProperty('OPENAI_BASE_URL')
    expect(env).not.toHaveProperty('ANTHROPIC_BASE_URL')
  })
})
