import type {
  RemoteImConfig,
  RemoteImValidationIssue,
  RemoteImValidationResult
} from './types.js'

export const DEFAULT_REMOTE_IM_CONFIG: RemoteImConfig = {
  enabled: false,
  provider: 'tencent-im',
  sdkAppId: null,
  desktopUserId: '',
  userSigEndpoint: '',
  allowedUserIds: [],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
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

export function normalizeRemoteImConfig(value: unknown): RemoteImConfig {
  if (!value || typeof value !== 'object') return { ...DEFAULT_REMOTE_IM_CONFIG }
  const raw = value as Partial<Record<keyof RemoteImConfig, unknown>>
  return {
    enabled: raw.enabled === true,
    provider: 'tencent-im',
    sdkAppId: normalizeSdkAppId(raw.sdkAppId),
    desktopUserId: normalizeString(raw.desktopUserId),
    userSigEndpoint: normalizeString(raw.userSigEndpoint),
    allowedUserIds: normalizeAllowedUserIds(raw.allowedUserIds),
    outputFlushIntervalMs: normalizeNumber(raw.outputFlushIntervalMs, 2000, 1000, 30_000),
    outputMaxChunkChars: normalizeNumber(raw.outputMaxChunkChars, 1200, 200, 4000)
  }
}

export function validateRemoteImConfig(config: RemoteImConfig): RemoteImValidationResult {
  const issues: RemoteImValidationIssue[] = []
  if (!config.enabled) return { ok: true, issues: [] }
  if (!config.sdkAppId) {
    issues.push({ path: 'sdkAppId', message: 'SDKAppID is required when remote IM is enabled' })
  }
  if (!config.desktopUserId) {
    issues.push({
      path: 'desktopUserId',
      message: 'desktop UserID is required when remote IM is enabled'
    })
  }
  if (!config.userSigEndpoint) {
    issues.push({
      path: 'userSigEndpoint',
      message: 'UserSig endpoint is required when remote IM is enabled'
    })
  }
  if (config.allowedUserIds.length === 0) {
    issues.push({
      path: 'allowedUserIds',
      message: 'at least one allowed mobile UserID is required'
    })
  }
  return issues.length === 0 ? { ok: true, issues: [] } : { ok: false, issues }
}
