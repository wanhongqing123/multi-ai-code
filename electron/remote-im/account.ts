import { promises as fs } from 'fs'
import { join } from 'path'
import type { RemoteDesktopMode, RemoteImAccountConfig, RemoteImConfig } from './types.js'

const ACCOUNT_FILE = 'remote-im-account.json'

export const DEFAULT_REMOTE_IM_ACCOUNT_CONFIG: RemoteImAccountConfig = {
  provider: 'tencent-im',
  sdkAppId: null,
  desktopUserId: '',
  userSigMode: 'endpoint',
  userSigEndpoint: '',
  userSigSecretKey: '',
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 4000,
  remoteDesktopMode: 'disabled',
  remoteDesktopControl: false
}

function normalizeNumberInRange(value: unknown, fallback: number, min: number, max: number): number {
  const parsed = typeof value === 'number' ? value : Number(value)
  if (!Number.isFinite(parsed)) return fallback
  return Math.min(max, Math.max(min, Math.round(parsed)))
}

function normalizeIntervalMs(value: unknown): number {
  return normalizeNumberInRange(value, 2000, 1000, 30_000)
}

function normalizeMaxChunkChars(value: unknown): number {
  return normalizeNumberInRange(value, 4000, 200, 20_000)
}

function normalizeDesktopMode(value: unknown): RemoteDesktopMode {
  return value === 'attended' || value === 'unattended' ? value : 'disabled'
}

function normalizeString(value: unknown): string {
  return typeof value === 'string' ? value.trim() : ''
}

function normalizeSdkAppId(value: unknown): number | null {
  if (typeof value === 'number' && Number.isInteger(value) && value > 0) return value
  if (typeof value === 'string') {
    const parsed = Number(value.trim())
    if (Number.isInteger(parsed) && parsed > 0) return parsed
  }
  return null
}

function normalizeUserIds(value: unknown): string[] {
  if (!Array.isArray(value)) return []
  return Array.from(
    new Set(value.map((item) => normalizeString(item)).filter((item) => item.length > 0))
  )
}

function mergeUserIds(...lists: string[][]): string[] {
  return Array.from(new Set(lists.flat()))
}

export function normalizeRemoteImAccountConfig(value: unknown): RemoteImAccountConfig {
  if (!value || typeof value !== 'object') return { ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG }
  const raw = value as Partial<Record<keyof RemoteImAccountConfig, unknown>>
  const userSigEndpoint = normalizeString(raw.userSigEndpoint)
  const userSigSecretKey = normalizeString(raw.userSigSecretKey)
  const sdkAppId = normalizeSdkAppId(raw.sdkAppId)
  const userSigMode =
    raw.userSigMode === 'secret-key' || (!raw.userSigMode && userSigSecretKey && !userSigEndpoint)
      ? 'secret-key'
      : 'endpoint'
  return {
    provider: 'tencent-im',
    sdkAppId,
    desktopUserId: normalizeString(raw.desktopUserId),
    userSigMode,
    userSigEndpoint,
    userSigSecretKey,
    outputFlushIntervalMs: normalizeIntervalMs(raw.outputFlushIntervalMs),
    outputMaxChunkChars: normalizeMaxChunkChars(raw.outputMaxChunkChars),
    remoteDesktopMode: normalizeDesktopMode(raw.remoteDesktopMode),
    // 只有显式 true 才算开：配置损坏时必须收紧，不能变成「默认可被操作」。
    remoteDesktopControl: raw.remoteDesktopControl === true
  }
}

export function mergeRemoteImAccountIntoConfig(
  projectConfig: RemoteImConfig,
  account: RemoteImAccountConfig
): RemoteImConfig {
  return {
    ...projectConfig,
    provider: account.provider,
    sdkAppId: account.sdkAppId,
    desktopUserId: account.desktopUserId,
    userSigMode: account.userSigMode,
    userSigEndpoint: account.userSigEndpoint,
    userSigSecretKey: account.userSigSecretKey,
    outputFlushIntervalMs: account.outputFlushIntervalMs,
    outputMaxChunkChars: account.outputMaxChunkChars,
    remoteDesktopMode: account.remoteDesktopMode,
    remoteDesktopControl: account.remoteDesktopControl
  }
}

export function hasRemoteImAccountConnectionChanged(
  previous: RemoteImAccountConfig,
  next: RemoteImAccountConfig
): boolean {
  const previousAccount = normalizeRemoteImAccountConfig(previous)
  const nextAccount = normalizeRemoteImAccountConfig(next)
  return (
    previousAccount.provider !== nextAccount.provider ||
    previousAccount.sdkAppId !== nextAccount.sdkAppId ||
    previousAccount.desktopUserId !== nextAccount.desktopUserId ||
    previousAccount.userSigMode !== nextAccount.userSigMode ||
    previousAccount.userSigEndpoint !== nextAccount.userSigEndpoint ||
    previousAccount.userSigSecretKey !== nextAccount.userSigSecretKey
  )
}

export async function readRemoteImAccountConfig(
  userDataDir: string
): Promise<RemoteImAccountConfig> {
  try {
    const raw = JSON.parse(await fs.readFile(join(userDataDir, ACCOUNT_FILE), 'utf8'))
    return normalizeRemoteImAccountConfig(raw)
  } catch {
    return { ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG }
  }
}

export async function writeRemoteImAccountConfig(
  userDataDir: string,
  account: RemoteImAccountConfig
): Promise<RemoteImAccountConfig> {
  const normalized = normalizeRemoteImAccountConfig(account)
  await fs.mkdir(userDataDir, { recursive: true })
  await fs.writeFile(join(userDataDir, ACCOUNT_FILE), JSON.stringify(normalized, null, 2), 'utf8')
  return normalized
}
