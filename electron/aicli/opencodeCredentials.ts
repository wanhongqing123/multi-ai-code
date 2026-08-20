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

const OPENCODE_CREDENTIALS_FILE = 'managed-credentials.json'
const MAX_API_KEY_LENGTH = 4096

interface OpenCodeCredentialsFile {
  version: 1
  zhipuApiKey: string
}

export function readOpenCodeApiKey(runtimeRoot: string): string {
  const path = join(runtimeRoot, OPENCODE_CREDENTIALS_FILE)
  if (!existsSync(path)) return ''

  let value: unknown
  try {
    value = JSON.parse(readFileSync(path, 'utf8'))
  } catch {
    throw new Error('OpenCode 智谱 API Key 配置已损坏，请在设置中重新保存')
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('OpenCode 智谱 API Key 配置格式无效，请在设置中重新保存')
  }
  const credentials = value as Partial<OpenCodeCredentialsFile>
  if (credentials.version !== 1 || typeof credentials.zhipuApiKey !== 'string') {
    throw new Error('OpenCode 智谱 API Key 配置格式无效，请在设置中重新保存')
  }
  return credentials.zhipuApiKey.trim()
}

export function writeOpenCodeApiKey(runtimeRoot: string, apiKey: string): void {
  const normalized = apiKey.trim()
  if (normalized.length > MAX_API_KEY_LENGTH) {
    throw new Error('智谱 API Key 长度无效')
  }

  mkdirSync(runtimeRoot, { recursive: true })
  const path = join(runtimeRoot, OPENCODE_CREDENTIALS_FILE)
  if (!normalized) {
    rmSync(path, { force: true })
    return
  }

  const temporaryPath = join(runtimeRoot, `.${OPENCODE_CREDENTIALS_FILE}.${randomUUID()}.tmp`)
  writeFileSync(
    temporaryPath,
    `${JSON.stringify({ version: 1, zhipuApiKey: normalized } satisfies OpenCodeCredentialsFile, null, 2)}\n`,
    { encoding: 'utf8', mode: 0o600 }
  )
  renameSync(temporaryPath, path)
  // rename 会保留临时文件权限；再 chmod 一次，覆盖旧文件或平台差异时仍保持仅当前用户可读。
  try {
    chmodSync(path, 0o600)
  } catch {
    // Windows 不完整支持 POSIX mode；文件仍位于当前桌面账号的私有数据目录。
  }
}

export function readOpenCodeCredentialEnv(runtimeRoot: string): Record<string, string> {
  const apiKey = readOpenCodeApiKey(runtimeRoot)
  return apiKey ? { ZHIPU_API_KEY: apiKey } : {}
}
