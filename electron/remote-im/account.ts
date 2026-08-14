import { promises as fs } from 'fs'
import { join } from 'path'
import type { RemoteImAccountConfig, RemoteImConfig } from './types.js'
import { migrateRemoteImCredential } from './credentials.js'

const ACCOUNT_FILE = 'remote-im-account.json'

export const DEFAULT_REMOTE_IM_ACCOUNT_CONFIG: RemoteImAccountConfig = {
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
  blockedUserIds: []
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
    account.masterUserIds.length > 0 ||
    account.slaveUserIds.length > 0 ||
    account.allowedUserIds.length > 0
  )
}

export function normalizeRemoteImAccountConfig(value: unknown): RemoteImAccountConfig {
  if (!value || typeof value !== 'object') return { ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG }
  const raw = value as Partial<Record<keyof RemoteImAccountConfig, unknown>>
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
  const blockedUserIds = normalizeUserIds(raw.blockedUserIds)
  const blockedUserIdSet = new Set(blockedUserIds)
  const friendUserIds = mergeUserIds(
    normalizeUserIds(raw.friendUserIds),
    normalizeUserIds(raw.masterUserIds),
    normalizeUserIds(raw.slaveUserIds),
    normalizeUserIds(raw.allowedUserIds)
  ).filter((userId) => !blockedUserIdSet.has(userId))

  return {
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
    allowedUserIds: [...friendUserIds],
    blockedUserIds
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
    masterUserIds: previous.masterUserIds,
    slaveUserIds: previous.slaveUserIds,
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
    masterUserIds: [],
    slaveUserIds: [],
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
    masterUserIds: removeUserId(previous.masterUserIds),
    slaveUserIds: removeUserId(previous.slaveUserIds),
    allowedUserIds: removeUserId(previous.allowedUserIds),
    blockedUserIds: Array.from(new Set([...(previous.blockedUserIds ?? []), userId]))
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
    desktopRole: account.desktopRole,
    userSigMode: account.userSigMode,
    userSigEndpoint: account.userSigEndpoint,
    userSigSecretKey: account.userSigSecretKey,
    friendUserIds: account.friendUserIds,
    masterUserIds: account.masterUserIds,
    slaveUserIds: account.slaveUserIds,
    allowedUserIds: account.allowedUserIds
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
