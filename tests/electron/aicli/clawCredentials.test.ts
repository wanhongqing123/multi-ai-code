import { describe, expect, it } from 'vitest'
import { mkdtempSync, rmSync, existsSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  readClawConfig,
  writeClawConfig,
  withClawRuntimeEnv,
  withClawRuntimeModelArgs
} from '../../../electron/aicli/clawCredentials.js'

describe('claw config round trip', () => {
  it('writes, reads back, and produces the launch env proven against a live endpoint', () => {
    const dir = mkdtempSync(join(tmpdir(), 'claw-cfg-'))
    try {
      writeClawConfig(dir, { apiKey: 'zhipu-abc', baseUrl: '', model: '' })
      expect(readClawConfig(dir)).toEqual({ apiKey: 'zhipu-abc', baseUrl: '', model: '' })

      expect(withClawRuntimeEnv('claw', undefined, dir)).toEqual({
        OPENAI_API_KEY: 'zhipu-abc',
        OPENAI_BASE_URL: 'https://open.bigmodel.cn/api/paas/v4'
      })
      expect(withClawRuntimeModelArgs('claw', [], dir)).toEqual(['--model', 'openai/glm-4.6'])

      // 清空 Key 等于取消配置：文件整份删掉。
      writeClawConfig(dir, { apiKey: '', baseUrl: 'https://x', model: 'y' })
      expect(existsSync(join(dir, 'managed-config.json'))).toBe(false)
      expect(readClawConfig(dir)).toEqual({ apiKey: '', baseUrl: '', model: '' })
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})
