import { describe, expect, it } from 'vitest'
import { mkdtempSync, rmSync, existsSync, readFileSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  readClawConfig,
  writeClawConfig,
  withClawRuntimeEnv,
  withClawRuntimeModelArgs
} from '../../../electron/aicli/clawCredentials.js'
import {
  CLAW_DEFAULT_MODEL,
  CLAW_DEFAULT_PROTOCOL,
  EMPTY_CLAW_CONFIG,
  clawProtocolSpec
} from '../../../electron/aicli/clawConfig.js'

function withTempDir(body: (dir: string) => void): void {
  const dir = mkdtempSync(join(tmpdir(), 'claw-cfg-'))
  try {
    body(dir)
  } finally {
    rmSync(dir, { recursive: true, force: true })
  }
}

describe('claw config round trip', () => {
  it('writes, reads back, and produces the launch env for the selected protocol', () => {
    withTempDir((dir) => {
      writeClawConfig(dir, {
        protocol: 'anthropic',
        apiKey: 'zhipu-abc',
        baseUrl: '',
        model: 'glm-5.3',
        subagentModel: ''
      })
      expect(readClawConfig(dir)).toEqual({
        protocol: 'anthropic',
        apiKey: 'zhipu-abc',
        baseUrl: '',
        model: 'glm-5.3',
        subagentModel: ''
      })

      // 端点与 --model 必须一起落在 Anthropic 那一组上。
      expect(withClawRuntimeEnv('claw', undefined, dir)).toEqual({
        ANTHROPIC_API_KEY: 'zhipu-abc',
        ANTHROPIC_BASE_URL: clawProtocolSpec('anthropic').defaultBaseUrl,
        CLAW_MODEL: 'anthropic/glm-5.3'
      })
      expect(withClawRuntimeModelArgs('claw', [], dir)).toEqual(['--model', 'anthropic/glm-5.3'])
    })
  })

  it('switching protocol switches which env pair is written', () => {
    withTempDir((dir) => {
      writeClawConfig(dir, {
        protocol: 'openai',
        apiKey: 'k',
        baseUrl: '',
        model: '',
        subagentModel: ''
      })
      expect(withClawRuntimeEnv('claw', undefined, dir)).toEqual({
        OPENAI_API_KEY: 'k',
        OPENAI_BASE_URL: clawProtocolSpec('openai').defaultBaseUrl,
        CLAW_MODEL: `openai/${CLAW_DEFAULT_MODEL}`
      })
      expect(withClawRuntimeModelArgs('claw', [], dir)).toEqual([
        '--model',
        `openai/${CLAW_DEFAULT_MODEL}`
      ])
    })
  })

  // 协议是磁盘上的一等字段，不能只靠模型前缀捎带。裸模型名 + 非默认协议是
  // 最容易漏的组合：不落盘的话，下次读回来会掉回默认协议。
  it('persists the protocol even when the model name carries no prefix', () => {
    withTempDir((dir) => {
      writeClawConfig(dir, {
        protocol: 'xai',
        apiKey: 'k',
        baseUrl: '',
        model: 'grok-4',
        subagentModel: ''
      })
      const raw = JSON.parse(readFileSync(join(dir, 'managed-config.json'), 'utf8'))
      expect(raw.protocol).toBe('xai')
      expect(readClawConfig(dir).protocol).toBe('xai')
    })
  })

  // 老配置没有 protocol 字段，只有带前缀的全名。升级后必须落回同一个服务商，
  // 并把界面上显示的模型名还原成裸名。
  it('migrates a v1 config that only has a prefixed model name', () => {
    withTempDir((dir) => {
      writeFileSync(
        join(dir, 'managed-config.json'),
        JSON.stringify({
          version: 1,
          apiKey: 'legacy-key',
          baseUrl: 'https://open.bigmodel.cn/api/anthropic',
          model: 'anthropic/glm-5.3'
        }),
        'utf8'
      )
      expect(readClawConfig(dir)).toEqual({
        protocol: 'anthropic',
        apiKey: 'legacy-key',
        baseUrl: 'https://open.bigmodel.cn/api/anthropic',
        model: 'glm-5.3',
        // 老配置没有这个字段，缺失等同「跟随主模型」。
        subagentModel: ''
      })
      expect(withClawRuntimeEnv('claw', undefined, dir)).toEqual({
        ANTHROPIC_API_KEY: 'legacy-key',
        ANTHROPIC_BASE_URL: 'https://open.bigmodel.cn/api/anthropic',
        CLAW_MODEL: 'anthropic/glm-5.3'
      })
    })
  })

  it('falls back to the default protocol for a legacy bare model name', () => {
    withTempDir((dir) => {
      writeFileSync(
        join(dir, 'managed-config.json'),
        JSON.stringify({ version: 1, apiKey: 'k', baseUrl: '', model: 'glm-5.3' }),
        'utf8'
      )
      expect(readClawConfig(dir).protocol).toBe(CLAW_DEFAULT_PROTOCOL)
    })
  })

  it('treats an unknown protocol string on disk as absent', () => {
    withTempDir((dir) => {
      writeFileSync(
        join(dir, 'managed-config.json'),
        JSON.stringify({
          version: 1,
          protocol: 'not-a-protocol',
          apiKey: 'k',
          baseUrl: '',
          model: 'anthropic/glm-5.3'
        }),
        'utf8'
      )
      expect(readClawConfig(dir).protocol).toBe('anthropic')
    })
  })

  it('round-trips a dedicated subagent model and exports it to claw', () => {
    withTempDir((dir) => {
      writeClawConfig(dir, {
        protocol: 'openai',
        apiKey: 'k',
        baseUrl: '',
        model: 'glm-5.3',
        subagentModel: 'glm-5.3-flash'
      })
      expect(readClawConfig(dir).subagentModel).toBe('glm-5.3-flash')
      const env = withClawRuntimeEnv('claw', undefined, dir)
      expect(env?.CLAW_MODEL).toBe('openai/glm-5.3')
      expect(env?.CLAW_SUBAGENT_MODEL).toBe('openai/glm-5.3-flash')
    })
  })

  // 留空就不该注入专用变量，让 claw 自己回退到 CLAW_MODEL。
  it('exports no subagent variable when the field is left empty', () => {
    withTempDir((dir) => {
      writeClawConfig(dir, {
        protocol: 'openai',
        apiKey: 'k',
        baseUrl: '',
        model: 'glm-5.3',
        subagentModel: ''
      })
      const env = withClawRuntimeEnv('claw', undefined, dir)
      expect(env).not.toHaveProperty('CLAW_SUBAGENT_MODEL')
      expect(env?.CLAW_MODEL).toBe('openai/glm-5.3')
    })
  })

  it('clearing the key deletes the file rather than leaving an endpoint-only shell', () => {
    withTempDir((dir) => {
      writeClawConfig(dir, {
        protocol: 'openai',
        apiKey: 'k',
        baseUrl: '',
        model: '',
        subagentModel: ''
      })
      writeClawConfig(dir, {
        protocol: 'openai',
        apiKey: '',
        baseUrl: 'https://x',
        model: 'y',
        subagentModel: 'z'
      })
      expect(existsSync(join(dir, 'managed-config.json'))).toBe(false)
      expect(readClawConfig(dir)).toEqual(EMPTY_CLAW_CONFIG)
    })
  })

  it('rejects a Base URL that is not http(s)', () => {
    withTempDir((dir) => {
      expect(() =>
        writeClawConfig(dir, {
          protocol: 'openai',
          apiKey: 'k',
          baseUrl: 'ftp://example.com',
          model: '',
          subagentModel: ''
        })
      ).toThrow(/http/)
    })
  })

  it('reports a corrupted config instead of silently starting unconfigured', () => {
    withTempDir((dir) => {
      writeFileSync(join(dir, 'managed-config.json'), '{ not json', 'utf8')
      expect(() => readClawConfig(dir)).toThrow(/损坏/)
    })
  })
})
