import type { RemoteImConfig, RemoteImContactRelation, RemoteImDesktopRole } from './types.js'

export type RemoteImPeerRole = RemoteImDesktopRole
export type RemoteImPeerRelation = RemoteImContactRelation

export type RemoteImInboundTaskDeniedReason = 'sender-not-allowed'
export type RemoteImManualSendDeniedReason = 'peer-not-allowed'

export type RemoteImPermissionResult<TReason extends string> =
  | { ok: true; peerRole: RemoteImPeerRole }
  | { ok: false; reason: TReason; peerRole: RemoteImPeerRole | null }

function masterUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.masterUserIds) ? config.masterUserIds : config.allowedUserIds ?? []
}

function slaveUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.slaveUserIds) ? config.slaveUserIds : []
}

function friendUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.friendUserIds) ? config.friendUserIds : []
}

function trustedFriendUserIds(config: RemoteImConfig): string[] {
  return Array.from(
    new Set([
      ...friendUserIds(config),
      ...masterUserIds(config),
      ...slaveUserIds(config),
      ...(Array.isArray(config.allowedUserIds) ? config.allowedUserIds : [])
    ])
  )
}

export function getRemoteImPeerRelation(
  config: RemoteImConfig,
  userId: string
): RemoteImPeerRelation | null {
  const cleanUserId = userId.trim()
  if (!cleanUserId) return null
  return trustedFriendUserIds(config).includes(cleanUserId) ? 'friend' : null
}

export function getRemoteImPeerRole(
  config: RemoteImConfig,
  userId: string
): RemoteImPeerRole | null {
  void config
  void userId
  return null
}

export function canRouteRemoteImTaskFrom(
  config: RemoteImConfig,
  fromUserId: string
): RemoteImPermissionResult<RemoteImInboundTaskDeniedReason> {
  if (!getRemoteImPeerRelation(config, fromUserId)) {
    return { ok: false, reason: 'sender-not-allowed', peerRole: null }
  }
  return { ok: true, peerRole: 'master' }
}

export function canManuallySendToRemoteImPeer(
  config: RemoteImConfig,
  toUserId: string
): RemoteImPermissionResult<RemoteImManualSendDeniedReason> {
  const peerRelation = getRemoteImPeerRelation(config, toUserId)
  if (!peerRelation) return { ok: false, reason: 'peer-not-allowed', peerRole: null }
  return {
    ok: true,
    peerRole: 'master'
  }
}

export function resolveDefaultRemoteImPeerUserId(config: RemoteImConfig): string | null {
  return friendUserIds(config)[0] ?? masterUserIds(config)[0] ?? slaveUserIds(config)[0] ?? null
}
