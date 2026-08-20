import type {
  RemoteDesktopMode,
  RemoteImConfig,
  RemoteImValidationIssue,
  RemoteImValidationResult
} from './types.js'

export const DEFAULT_REMOTE_IM_CONFIG: RemoteImConfig = {
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
  outputMaxChunkChars: 4000,
  remoteDesktopMode: 'disabled',
  remoteDesktopControl: false
}

const LEGACY_DEFAULT_OUTPUT_MAX_CHUNK_CHARS = 1200

const REMOTE_DESKTOP_MODES: readonly RemoteDesktopMode[] = ['disabled', 'attended', 'unattended']

/**
 * 无法识别的值一律回落 disabled。配置损坏时必须收紧而不是放宽——
 * 解析失败绝不能变成"屏幕默认可被查看"。
 */
function normalizeRemoteDesktopMode(value: unknown): RemoteDesktopMode {
  return REMOTE_DESKTOP_MODES.includes(value as RemoteDesktopMode)
    ? (value as RemoteDesktopMode)
    : 'disabled'
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

function normalizeOutputMaxChunkChars(value: unknown): number {
  const normalized = normalizeNumber(value, 4000, 200, 4000)
  return normalized === LEGACY_DEFAULT_OUTPUT_MAX_CHUNK_CHARS ? 4000 : normalized
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
  const sdkAppId = normalizeSdkAppId(raw.sdkAppId)
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
    provider: 'tencent-im',
    sdkAppId,
    desktopUserId: normalizeString(raw.desktopUserId),
    desktopRole: 'master',
    userSigMode,
    userSigEndpoint,
    userSigSecretKey,
    friendUserIds,
    masterUserIds: [],
    slaveUserIds: [],
    allowedUserIds,
    outputFlushIntervalMs: normalizeNumber(raw.outputFlushIntervalMs, 2000, 1000, 30_000),
    outputMaxChunkChars: normalizeOutputMaxChunkChars(raw.outputMaxChunkChars),
    remoteDesktopMode: normalizeRemoteDesktopMode(raw.remoteDesktopMode),
    // 只有显式 true 才算开：配置损坏时必须收紧，不能变成"默认可被操作"。
    remoteDesktopControl: raw.remoteDesktopControl === true
  }
}

export function validateRemoteImConfig(config: RemoteImConfig): RemoteImValidationResult {
  void config
  const issues: RemoteImValidationIssue[] = []
  return issues.length === 0 ? { ok: true, issues: [] } : { ok: false, issues }
}

/** 项目 meta 里真正属于项目级的那几个键（其余读取时由账号库覆盖）。 */
export interface RemoteImProjectMetaConfig {
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
  remoteDesktopMode: RemoteDesktopMode
  remoteDesktopControl: boolean
}

/**
 * 落盘用的最小项目配置。
 *
 * 以前是把整个 RemoteImConfig 写进 project.json，于是每个项目里都躺着一份
 * sdkAppId/desktopUserId/friendUserIds 的空壳——读取时全被账号库覆盖，纯冗余，
 * 还容易让人误以为「这些能按项目改」。
 */
export function toRemoteImProjectMetaConfig(
  config: RemoteImConfig
): RemoteImProjectMetaConfig {
  return {
    outputFlushIntervalMs: config.outputFlushIntervalMs,
    outputMaxChunkChars: config.outputMaxChunkChars,
    remoteDesktopMode: config.remoteDesktopMode,
    remoteDesktopControl: config.remoteDesktopControl
  }
}

export function toRemoteImProjectConfig(config: RemoteImConfig): RemoteImConfig {
  return {
    ...DEFAULT_REMOTE_IM_CONFIG,
    outputFlushIntervalMs: config.outputFlushIntervalMs,
    outputMaxChunkChars: config.outputMaxChunkChars,
    // 必须显式透传：这个函数以 DEFAULT 为底，漏掉就会在每次保存时把用户
    // 开启过的远程桌面悄悄重置回 disabled。
    remoteDesktopMode: config.remoteDesktopMode,
    remoteDesktopControl: config.remoteDesktopControl
  }
}
