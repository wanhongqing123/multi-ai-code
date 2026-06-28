import { promises as fs } from 'fs'
import { join } from 'path'
import type { RemoteImAccountConfig, RemoteImConfig } from './types.js'

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
  allowedUserIds: []
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
  const userSigMode =
    raw.userSigMode === 'secret-key' || (!raw.userSigMode && userSigSecretKey && !userSigEndpoint)
      ? 'secret-key'
      : 'endpoint'
  const friendUserIds = normalizeUserIds(raw.friendUserIds)
  const masterUserIds = normalizeUserIds(raw.masterUserIds)
  const slaveUserIds = normalizeUserIds(raw.slaveUserIds)

  return {
    provider: 'tencent-im',
    sdkAppId: normalizeSdkAppId(raw.sdkAppId),
    desktopUserId: normalizeString(raw.desktopUserId),
    desktopRole: raw.desktopRole === 'slave' ? 'slave' : 'master',
    userSigMode,
    userSigEndpoint,
    userSigSecretKey,
    friendUserIds,
    masterUserIds,
    slaveUserIds,
    allowedUserIds: mergeUserIds(friendUserIds, masterUserIds, slaveUserIds)
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
