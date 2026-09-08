import { describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import {
  CLAW_DEFAULT_MODEL,
  CLAW_DEFAULT_PROTOCOL,
  CLAW_PROTOCOLS,
  EMPTY_CLAW_CONFIG,
  clawBareModel,
  clawProtocolFromModel,
  clawProtocolSpec,
  clawQualifiedModel,
  clawQualifiedSubagentModel,
  isClawCommand,
  withClawDataDirEnv,
  withClawManagedEnv,
  withClawModelArgs,
  type ClawManagedConfig
} from '../../../electron/aicli/clawConfig.js'

function config(overrides: Partial<ClawManagedConfig> = {}): ClawManagedConfig {
  return { ...EMPTY_CLAW_CONFIG, apiKey: 'k', ...overrides }
}

describe('isClawCommand', () => {
  it('matches the bare claw binary in any command form', () => {
    expect(isClawCommand('claw')).toBe(true)
    expect(isClawCommand('claw.exe')).toBe(true)
    expect(isClawCommand('"claw"')).toBe(true)
    expect(isClawCommand('C:\\Tools\\claw.exe')).toBe(true)
    expect(isClawCommand('/usr/local/bin/claw')).toBe(true)
  })

  // claw workspace 里真实存在 claw-analog / claw-rag-service；判定少了 $ 锚点
  // 会把它们当成 claw，注入错误的 env 和 --model。
  it('does not match the other binaries in the claw workspace', () => {
    expect(isClawCommand('claw-analog')).toBe(false)
    expect(isClawCommand('claw-rag-service')).toBe(false)
    expect(isClawCommand('clawx')).toBe(false)
    expect(isClawCommand('claude')).toBe(false)
  })
})

// claw 的配置只对自己负责。**刻意不读 resources/opencode/** —— opencode 即将废弃，
// 它的资源文件和「宿主维护一份策展白名单、用户只能从里面挑」的设计思路都不该被 claw 继承。
describe('claw config is self-contained', () => {
  it('has no opencode in its code (the comments may explain why)', () => {
    const source = readFileSync(join(process.cwd(), 'electron', 'aicli', 'clawConfig.ts'), 'utf8')
    // 注释里写明「为什么不跟 opencode 那套」是有价值的，要禁的是**代码上的耦合**：
    // 引用它的资源文件、路径或模块。所以先把注释去掉再断言。
    const code = source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '')
    expect(code).not.toMatch(/opencode/i)
    expect(code).not.toMatch(/managed-models\.json/)
  })

  it('gives every protocol a prefix, an endpoint env and a sample model', () => {
    for (const spec of CLAW_PROTOCOLS) {
      expect(spec.prefix.endsWith('/')).toBe(true)
      expect(spec.baseUrlEnv).toMatch(/^[A-Z_]+$/)
      expect(spec.sampleModel.length).toBeGreaterThan(0)
      // 前缀必须能被反查回同一个协议，否则老配置迁移会错位。
      expect(clawProtocolFromModel(`${spec.prefix}whatever`)).toBe(spec.id)
    }
  })

  // Coding Plan 的 Key 只在 /coding/ 端点上有资源包；少了这一段实测返回 429。
  it('points the OpenAI-compatible default at the coding-plan endpoint', () => {
    expect(clawProtocolSpec('openai').defaultBaseUrl).toContain('/coding/')
  })

  it('lets ollama work without an API key', () => {
    expect(clawProtocolSpec('ollama').keyEnv).toBeNull()
  })
})

// 这一组钉的是一次真实事故：用户把 Base URL 填成智谱的 Anthropic 端点、模型填裸名
// glm-5.3，claw 报 404 NOT_FOUND。原因是代码无论走哪个协议都把 Base URL 写进
// OPENAI_BASE_URL，请求打到 <anthropic 端点>/chat/completions 这个不存在的路径。
describe('credentials and endpoint follow the selected protocol', () => {
  it('writes only the Anthropic env pair for the Anthropic protocol', () => {
    const env = withClawManagedEnv('claw', undefined, config({ protocol: 'anthropic' }))
    expect(env).toEqual({
      ANTHROPIC_API_KEY: 'k',
      ANTHROPIC_BASE_URL: 'https://open.bigmodel.cn/api/anthropic',
      CLAW_MODEL: `anthropic/${CLAW_DEFAULT_MODEL}`
    })
    // 事故的形状：绝不能把 Anthropic 端点写进 OPENAI_BASE_URL。
    expect(env).not.toHaveProperty('OPENAI_BASE_URL')
  })

  it('writes only the OpenAI env pair for the OpenAI protocol', () => {
    const env = withClawManagedEnv('claw', undefined, config({ protocol: 'openai' }))
    expect(env).toEqual({
      OPENAI_API_KEY: 'k',
      OPENAI_BASE_URL: 'https://open.bigmodel.cn/api/coding/paas/v4',
      CLAW_MODEL: `openai/${CLAW_DEFAULT_MODEL}`
    })
    expect(env).not.toHaveProperty('ANTHROPIC_BASE_URL')
  })

  it('uses each protocol own env names', () => {
    expect(withClawManagedEnv('claw', undefined, config({ protocol: 'xai' }))).toEqual({
      XAI_API_KEY: 'k',
      CLAW_MODEL: `xai/${CLAW_DEFAULT_MODEL}`
    })
    expect(withClawManagedEnv('claw', undefined, config({ protocol: 'dashscope' }))).toEqual({
      DASHSCOPE_API_KEY: 'k',
      CLAW_MODEL: `qwen/${CLAW_DEFAULT_MODEL}`
    })
  })

  it('injects the ollama endpoint even with no API key', () => {
    expect(
      withClawManagedEnv('claw', undefined, { ...EMPTY_CLAW_CONFIG, protocol: 'ollama' })
    ).toEqual({
      OLLAMA_HOST: 'http://127.0.0.1:11434/v1',
      CLAW_MODEL: `local/${CLAW_DEFAULT_MODEL}`
    })
  })

  it('honours a Base URL the user typed in over the protocol default', () => {
    const env = withClawManagedEnv(
      'claw',
      undefined,
      config({ protocol: 'openai', baseUrl: 'https://my-gateway.internal/v1' })
    )
    expect(env).toEqual({
      OPENAI_API_KEY: 'k',
      OPENAI_BASE_URL: 'https://my-gateway.internal/v1',
      CLAW_MODEL: `openai/${CLAW_DEFAULT_MODEL}`
    })
  })

  // 手写的 env 是用户指向别的服务的出口，托管配置绝不能盖掉。
  it('never overrides env the user set explicitly', () => {
    const userEnv = {
      OPENAI_API_KEY: 'mine',
      OPENAI_BASE_URL: 'http://localhost:1234/v1',
      CLAW_MODEL: 'openai/my-own-model',
      CLAW_SUBAGENT_MODEL: 'openai/my-own-cheap-model'
    }
    expect(withClawManagedEnv('claw', userEnv, config({ subagentModel: 'glm-5.3-flash' }))).toEqual(
      userEnv
    )
  })

  it('adds nothing when a key-requiring protocol has no key', () => {
    expect(withClawManagedEnv('claw', undefined, EMPTY_CLAW_CONFIG)).toEqual({})
  })

  it('is an identity transform for other CLIs', () => {
    const env = { FOO: 'bar' }
    expect(withClawManagedEnv('codex', env, config())).toBe(env)
    expect(withClawManagedEnv('claw-analog', env, config())).toBe(env)
  })
})

describe('model name is composed from protocol + bare name', () => {
  it('prefixes the bare model with the selected protocol', () => {
    expect(clawQualifiedModel(config({ protocol: 'openai', model: 'glm-5.3' }))).toBe(
      'openai/glm-5.3'
    )
    expect(clawQualifiedModel(config({ protocol: 'anthropic', model: 'glm-5.3' }))).toBe(
      'anthropic/glm-5.3'
    )
  })

  it('falls back to the default model when the field is empty', () => {
    expect(clawQualifiedModel(config({ protocol: 'openai', model: '' }))).toBe(
      `openai/${CLAW_DEFAULT_MODEL}`
    )
  })

  // 高级用户可以直接粘贴带前缀的全名，不该被再套一层。
  it('leaves an already-qualified name untouched', () => {
    expect(clawQualifiedModel(config({ protocol: 'openai', model: 'anthropic/glm-5.3' }))).toBe(
      'anthropic/glm-5.3'
    )
  })

  it('round-trips through the bare form shown in the settings UI', () => {
    expect(clawBareModel('openai/glm-5.3')).toBe('glm-5.3')
    expect(clawBareModel('glm-5.3')).toBe('glm-5.3')
  })

  // 老配置没有 protocol 字段，把带前缀的全名存在 model 里。升级后必须还原成同一个
  // 协议，否则用户既有配置会静默换服务商。
  it('infers the protocol from a legacy prefixed model name', () => {
    expect(clawProtocolFromModel('anthropic/glm-5.3')).toBe('anthropic')
    expect(clawProtocolFromModel('openai/glm-5.3')).toBe('openai')
    expect(clawProtocolFromModel('glm-5.3')).toBe(CLAW_DEFAULT_PROTOCOL)
  })

  // env 与 --model 必须落在同一个服务商上，否则又回到「凭据一边、端点另一边」的事故。
  it('keeps env and --model on the same protocol', () => {
    const anthropic = config({ protocol: 'anthropic', model: 'glm-5.3' })
    expect(withClawModelArgs('claw', [], anthropic)).toEqual(['--model', 'anthropic/glm-5.3'])
    expect(Object.keys(withClawManagedEnv('claw', undefined, anthropic) ?? {})).toEqual([
      'ANTHROPIC_API_KEY',
      'ANTHROPIC_BASE_URL',
      'CLAW_MODEL'
    ])
  })

  it('never touches an explicit --model, and skips other CLIs', () => {
    expect(withClawModelArgs('claw', ['--model', 'x'], config())).toEqual(['--model', 'x'])
    expect(withClawModelArgs('claw', ['--model=x'], config())).toEqual(['--model=x'])
    expect(withClawModelArgs('claw', ['--verbose'], EMPTY_CLAW_CONFIG)).toEqual(['--verbose'])
    expect(withClawModelArgs('codex', ['resume'], config())).toEqual(['resume'])
  })
})

// claw 的子代理（Agent 工具）上游把模型写死成 claude-opus-4-6。配智谱时一旦触发子代理，
// 就会拿用户的 Key 去请求一个对方根本没有的模型名。fork 里拆掉了那个常量，
// 改成读 CLAW_SUBAGENT_MODEL → CLAW_MODEL → ANTHROPIC_MODEL → ANTHROPIC_DEFAULT_MODEL。
// 这一组盯的是宿主这侧必须把这两个变量喂对。
describe('subagent model', () => {
  it('always exports the main model so the subagent can follow it', () => {
    // 子代理拿不到命令行参数，--model 对它不可见，只能从 env 看到主模型。
    const env = withClawManagedEnv('claw', undefined, config({ model: 'glm-5.3' }))
    expect(env?.CLAW_MODEL).toBe('openai/glm-5.3')
    expect(env?.CLAW_MODEL).toBe(withClawModelArgs('claw', [], config({ model: 'glm-5.3' }))[1])
  })

  it('exports a dedicated subagent model only when one is filled in', () => {
    expect(withClawManagedEnv('claw', undefined, config())).not.toHaveProperty(
      'CLAW_SUBAGENT_MODEL'
    )
    const env = withClawManagedEnv(
      'claw',
      undefined,
      config({ model: 'glm-5.3', subagentModel: 'glm-5.3-flash' })
    )
    expect(env?.CLAW_SUBAGENT_MODEL).toBe('openai/glm-5.3-flash')
  })

  it('qualifies the subagent model with the same protocol as the main model', () => {
    expect(
      clawQualifiedSubagentModel(config({ protocol: 'anthropic', subagentModel: 'glm-5.3-flash' }))
    ).toBe('anthropic/glm-5.3-flash')
    // 粘贴全名不再套一层前缀。
    expect(
      clawQualifiedSubagentModel(config({ protocol: 'openai', subagentModel: 'qwen/qwen-max' }))
    ).toBe('qwen/qwen-max')
  })

  // 留空 = 跟随主模型。宿主不在这里替用户拿主意，避免两边各算一份。
  it('leaves the subagent model unset when the field is empty', () => {
    expect(clawQualifiedSubagentModel(config())).toBeNull()
    expect(clawQualifiedSubagentModel(config({ subagentModel: '   ' }))).toBeNull()
  })

  it('adds neither model variable when the protocol has no key', () => {
    expect(withClawManagedEnv('claw', undefined, EMPTY_CLAW_CONFIG)).toEqual({})
  })
})

describe('withClawDataDirEnv', () => {
  it('moves claw session storage out of the user repo', () => {
    expect(withClawDataDirEnv('claw', undefined, 'C:/data/aicli/claw')).toEqual({
      CLAW_DATA_DIR: 'C:/data/aicli/claw'
    })
  })

  // 与凭据分开注入是刻意的：仓库污染跟有没有配协议无关，没配 Key 也必须生效。
  it('applies even when nothing else is configured', () => {
    const base = withClawManagedEnv('claw', undefined, EMPTY_CLAW_CONFIG)
    expect(base).toEqual({})
    expect(withClawDataDirEnv('claw', base, 'C:/data')).toEqual({ CLAW_DATA_DIR: 'C:/data' })
  })

  it('never overrides an explicitly configured data dir', () => {
    const userEnv = { CLAW_DATA_DIR: 'D:/my/own/place' }
    expect(withClawDataDirEnv('claw', userEnv, 'C:/data')).toEqual(userEnv)
  })

  it('is an identity transform for other CLIs', () => {
    const env = { FOO: 'bar' }
    expect(withClawDataDirEnv('codex', env, 'C:/data')).toBe(env)
  })
})
