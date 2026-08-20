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
  friendUserIds: [],
  allowedUserIds: [],
  blockedUserIds: [],
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

function hasAccountContacts(account: RemoteImAccountConfig): boolean {
  return (
    account.friendUserIds.length > 0 ||
    account.allowedUserIds.length > 0
  )
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
  const blockedUserIds = normalizeUserIds(raw.blockedUserIds)
  const blockedUserIdSet = new Set(blockedUserIds)
  const friendUserIds = mergeUserIds(
    normalizeUserIds(raw.friendUserIds),
    normalizeUserIds(raw.allowedUserIds)
  ).filter((userId) => !blockedUserIdSet.has(userId))

  return {
    provider: 'tencent-im',
    sdkAppId,
    desktopUserId: normalizeString(raw.desktopUserId),
    userSigMode,
    userSigEndpoint,
    userSigSecretKey,
    friendUserIds,
    allowedUserIds: [...friendUserIds],
    blockedUserIds,
    outputFlushIntervalMs: normalizeIntervalMs(raw.outputFlushIntervalMs),
    outputMaxChunkChars: normalizeMaxChunkChars(raw.outputMaxChunkChars),
    remoteDesktopMode: normalizeDesktopMode(raw.remoteDesktopMode),
    // 只有显式 true 才算开：配置损坏时必须收紧，不能变成「默认可被操作」。
    remoteDesktopControl: raw.remoteDesktopControl === true
  }
}

export function preserveRemoteImAccountContacts(
  incoming: RemoteImAccountConfig,
  existing: RemoteImAccountConfig
): RemoteImAccountConfig {
  const next = normalizeRemoteImAccountConfig(incoming)
  const previous = normalizeRemoteImAccountConfig(existing)
  if (!next.desktopUserId || next.desktopUserId !== previous.desktopUserId) return next
  const blockedUserIds = Array.isArray(incoming.blockedUserIds)
    ? next.blockedUserIds
    : previous.blockedUserIds
  if (hasAccountContacts(next) || !hasAccountContacts(previous)) {
    return normalizeRemoteImAccountConfig({ ...next, blockedUserIds })
  }
  return normalizeRemoteImAccountConfig({
    ...next,
    friendUserIds: previous.friendUserIds,
    allowedUserIds: previous.allowedUserIds,
    blockedUserIds
  })
}

export function syncRemoteImAccountContactsFromSdk(
  account: RemoteImAccountConfig,
  sdkFriendUserIds: string[]
): RemoteImAccountConfig {
  const previous = normalizeRemoteImAccountConfig(account)
  const blockedUserIds = previous.blockedUserIds ?? []
  const blockedUserIdSet = new Set(blockedUserIds)
  const friendUserIds = normalizeUserIds(sdkFriendUserIds).filter(
    (userId) => !blockedUserIdSet.has(userId)
  )
  return normalizeRemoteImAccountConfig({
    ...previous,
    friendUserIds,
    allowedUserIds: friendUserIds,
    blockedUserIds
  })
}

export function removeRemoteImAccountContact(
  account: RemoteImAccountConfig,
  rawUserId: string
): RemoteImAccountConfig {
  const userId = rawUserId.trim()
  if (!userId) return normalizeRemoteImAccountConfig(account)
  const previous = normalizeRemoteImAccountConfig(account)
  const removeUserId = (userIds: string[]) => userIds.filter((item) => item !== userId)
  return normalizeRemoteImAccountConfig({
    ...previous,
    friendUserIds: removeUserId(previous.friendUserIds),
    allowedUserIds: removeUserId(previous.allowedUserIds),
    blockedUserIds: Array.from(new Set([...(previous.blockedUserIds ?? []), userId]))
  })
}

/**
 * 把一个账号加进好友名单。
 *
 * 必须同时把它从 blockedUserIds 里摘掉：那是「本地已删好友」的墓碑，留着的话
 * SDK 下次同步会重新把这个人过滤掉——表现为「加了但过一会儿又没了」。
 * 与 removeRemoteImAccountContact 互为逆操作。
 */
export function addRemoteImAccountContact(
  account: RemoteImAccountConfig,
  rawUserId: string
): RemoteImAccountConfig {
  const userId = rawUserId.trim()
  if (!userId) return normalizeRemoteImAccountConfig(account)
  const previous = normalizeRemoteImAccountConfig(account)
  return normalizeRemoteImAccountConfig({
    ...previous,
    friendUserIds: Array.from(new Set([...previous.friendUserIds, userId])),
    allowedUserIds: Array.from(new Set([...previous.allowedUserIds, userId])),
    blockedUserIds: (previous.blockedUserIds ?? []).filter((item) => item !== userId)
  })
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
    friendUserIds: account.friendUserIds,
    allowedUserIds: account.allowedUserIds,
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

/**
 * Return identities that lose trusted-friend authority after an account
 * contact update. Account normalization folds all legacy role lists into the
 * canonical friend list, so comparing that list covers every inbound route
 * permission used by the current product.
 */
export function removedRemoteImAccountContactUserIds(
  previous: RemoteImAccountConfig,
  next: RemoteImAccountConfig
): string[] {
  const previousUserIds = normalizeRemoteImAccountConfig(previous).friendUserIds
  const nextUserIds = new Set(normalizeRemoteImAccountConfig(next).friendUserIds)
  return previousUserIds.filter((userId) => !nextUserIds.has(userId))
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
