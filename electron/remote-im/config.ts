import type {
  RemoteImConfig,
  RemoteImValidationIssue,
  RemoteImValidationResult
} from './types.js'
import { migrateRemoteImCredential } from './credentials.js'

export const DEFAULT_REMOTE_IM_CONFIG: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: null,
  desktopUserId: '',
  desktopRole: 'master',
  userSigMode: 'endpoint',
  userSigEndpoint: '',
  userSigSecretKey: '',
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: [],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 4000
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

function normalizeNumber(value: unknown, fallback: number, min: number, max: number): number {
  const parsed = typeof value === 'number' ? value : Number(value)
  if (!Number.isFinite(parsed)) return fallback
  return Math.min(max, Math.max(min, Math.round(parsed)))
}

function normalizeAllowedUserIds(value: unknown): string[] {
  if (!Array.isArray(value)) return []
  const ids = value
    .map((item) => normalizeString(item))
    .filter((item) => item.length > 0)
  return Array.from(new Set(ids))
}

function mergeUserIds(...lists: string[][]): string[] {
  return Array.from(new Set(lists.flat()))
}

export function normalizeRemoteImConfig(value: unknown): RemoteImConfig {
  if (!value || typeof value !== 'object') return { ...DEFAULT_REMOTE_IM_CONFIG }
  const raw = value as Partial<Record<keyof RemoteImConfig, unknown>>
  const userSigEndpoint = normalizeString(raw.userSigEndpoint)
  const userSigSecretKey = normalizeString(raw.userSigSecretKey)
  const credential = migrateRemoteImCredential({
    sdkAppId: normalizeSdkAppId(raw.sdkAppId),
    userSigSecretKey
  })
  const userSigMode =
    raw.userSigMode === 'secret-key' || (!raw.userSigMode && userSigSecretKey && !userSigEndpoint)
      ? 'secret-key'
      : 'endpoint'
  const legacyAllowedUserIds = normalizeAllowedUserIds(raw.allowedUserIds)
  const friendUserIds = mergeUserIds(
    normalizeAllowedUserIds(raw.friendUserIds),
    normalizeAllowedUserIds(raw.masterUserIds),
    normalizeAllowedUserIds(raw.slaveUserIds),
    legacyAllowedUserIds
  )
  const allowedUserIds = [...friendUserIds]
  return {
    enabled: true,
    provider: 'tencent-im',
    sdkAppId: credential.sdkAppId,
    desktopUserId: normalizeString(raw.desktopUserId),
    desktopRole: 'master',
    userSigMode,
    userSigEndpoint,
    userSigSecretKey: credential.userSigSecretKey,
    friendUserIds,
    masterUserIds: [],
    slaveUserIds: [],
    allowedUserIds,
    outputFlushIntervalMs: normalizeNumber(raw.outputFlushIntervalMs, 2000, 1000, 30_000),
    outputMaxChunkChars: normalizeNumber(raw.outputMaxChunkChars, 4000, 200, 4000)
  }
}

export function validateRemoteImConfig(config: RemoteImConfig): RemoteImValidationResult {
  void config
  const issues: RemoteImValidationIssue[] = []
  return issues.length === 0 ? { ok: true, issues: [] } : { ok: false, issues }
}

export function toRemoteImProjectConfig(config: RemoteImConfig): RemoteImConfig {
  return {
    ...DEFAULT_REMOTE_IM_CONFIG,
    enabled: true,
    outputFlushIntervalMs: config.outputFlushIntervalMs,
    outputMaxChunkChars: config.outputMaxChunkChars
  }
}
