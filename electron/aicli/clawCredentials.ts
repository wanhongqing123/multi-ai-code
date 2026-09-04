import {
  chmodSync,
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync
} from 'fs'
import { randomUUID } from 'crypto'
import { join } from 'path'
import {
  EMPTY_CLAW_CONFIG,
  withClawManagedEnv,
  withClawModelArgs,
  type ClawManagedConfig
} from './clawConfig.js'

const CLAW_CONFIG_FILE = 'managed-config.json'
const MAX_API_KEY_LENGTH = 4096
const MAX_BASE_URL_LENGTH = 2048
const MAX_MODEL_LENGTH = 256

interface ClawConfigFile {
  version: 1
  apiKey: string
  baseUrl: string
  model: string
}

function corrupt(): never {
  throw new Error('Claw 配置已损坏，请在设置中重新保存')
}

export function readClawConfig(runtimeRoot: string): ClawManagedConfig {
  const path = join(runtimeRoot, CLAW_CONFIG_FILE)
  if (!existsSync(path)) return { ...EMPTY_CLAW_CONFIG }

  let value: unknown
  try {
    value = JSON.parse(readFileSync(path, 'utf8'))
  } catch {
    corrupt()
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) corrupt()

  const config = value as Partial<ClawConfigFile>
  if (config.version !== 1 || typeof config.apiKey !== 'string') corrupt()

  return {
    apiKey: config.apiKey.trim(),
    baseUrl: typeof config.baseUrl === 'string' ? config.baseUrl.trim() : '',
    model: typeof config.model === 'string' ? config.model.trim() : ''
  }
}

export function writeClawConfig(runtimeRoot: string, config: ClawManagedConfig): void {
  const apiKey = config.apiKey.trim()
  const baseUrl = config.baseUrl.trim()
  const model = config.model.trim()
  if (apiKey.length > MAX_API_KEY_LENGTH) throw new Error('Claw API Key 长度无效')
  if (baseUrl.length > MAX_BASE_URL_LENGTH) throw new Error('Claw Base URL 长度无效')
  if (model.length > MAX_MODEL_LENGTH) throw new Error('Claw 模型名长度无效')
  if (baseUrl && !/^https?:\/\//i.test(baseUrl)) {
    throw new Error('Claw Base URL 必须以 http:// 或 https:// 开头')
  }

  mkdirSync(runtimeRoot, { recursive: true })
  const path = join(runtimeRoot, CLAW_CONFIG_FILE)
  // 没有 Key 就等于没配置：整份删掉，而不是留一个只有端点的空壳。
  if (!apiKey) {
    rmSync(path, { force: true })
    return
  }

  const temporaryPath = join(runtimeRoot, `.${CLAW_CONFIG_FILE}.${randomUUID()}.tmp`)
  writeFileSync(
    temporaryPath,
    `${JSON.stringify({ version: 1, apiKey, baseUrl, model } satisfies ClawConfigFile, null, 2)}\n`,
    { encoding: 'utf8', mode: 0o600 }
  )
  renameSync(temporaryPath, path)
  // rename 保留临时文件权限；再 chmod 一次，覆盖旧文件或平台差异时仍保持仅当前用户可读。
  try {
    chmodSync(path, 0o600)
  } catch {
    // Windows 不完整支持 POSIX mode；文件仍位于当前桌面账号的私有数据目录。
  }
}

/** 从磁盘读出托管配置后合成启动环境；非 claw 命令是恒等变换。 */
export function withClawRuntimeEnv(
  command: string,
  env: Record<string, string> | undefined,
  runtimeRoot: string
): Record<string, string> | undefined {
  return withClawManagedEnv(command, env, readClawConfig(runtimeRoot))
}

/** 从磁盘读出托管配置后补 --model；非 claw 命令是恒等变换。 */
export function withClawRuntimeModelArgs(
  command: string,
  args: readonly string[],
  runtimeRoot: string
): string[] {
  return withClawModelArgs(command, args, readClawConfig(runtimeRoot))
}
