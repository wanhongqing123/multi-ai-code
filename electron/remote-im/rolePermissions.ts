import type { RemoteImConfig, RemoteImContactRelation, RemoteImDesktopRole } from './types.js'

export type RemoteImPeerRole = RemoteImDesktopRole
export type RemoteImPeerRelation = RemoteImContactRelation

export type RemoteImInboundTaskDeniedReason =
  | 'sender-not-allowed'
  | 'slave-cannot-initiate'
  | 'slave-to-slave-blocked'
export type RemoteImManualSendDeniedReason = 'peer-not-allowed' | 'slave-cannot-initiate'

export type RemoteImPermissionResult<TReason extends string> =
  | { ok: true; peerRole: RemoteImPeerRole }
  | { ok: false; reason: TReason; peerRole: RemoteImPeerRole | null }

function localRole(config: RemoteImConfig): RemoteImDesktopRole {
  return config.desktopRole === 'slave' ? 'slave' : 'master'
}

function masterUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.masterUserIds) ? config.masterUserIds : config.allowedUserIds ?? []
}

function slaveUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.slaveUserIds) ? config.slaveUserIds : []
}

function friendUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.friendUserIds) ? config.friendUserIds : []
}

export function getRemoteImPeerRelation(
  config: RemoteImConfig,
  userId: string
): RemoteImPeerRelation | null {
  const cleanUserId = userId.trim()
  if (!cleanUserId) return null
  if (friendUserIds(config).includes(cleanUserId)) return 'friend'
  if (masterUserIds(config).includes(cleanUserId)) return 'master'
  if (slaveUserIds(config).includes(cleanUserId)) return 'slave'
  return null
}

export function getRemoteImPeerRole(
  config: RemoteImConfig,
  userId: string
): RemoteImPeerRole | null {
  const relation = getRemoteImPeerRelation(config, userId)
  return relation === 'master' || relation === 'slave' ? relation : null
}

export function canRouteRemoteImTaskFrom(
  config: RemoteImConfig,
  fromUserId: string
): RemoteImPermissionResult<RemoteImInboundTaskDeniedReason> {
  const peerRole = getRemoteImPeerRole(config, fromUserId)
  if (!peerRole) return { ok: false, reason: 'sender-not-allowed', peerRole: null }
  if (peerRole === 'slave' && localRole(config) === 'master') {
    return { ok: false, reason: 'slave-cannot-initiate', peerRole }
  }
  if (localRole(config) === 'slave' && peerRole === 'slave') {
    return { ok: false, reason: 'slave-to-slave-blocked', peerRole }
  }
  return { ok: true, peerRole }
}

export function canManuallySendToRemoteImPeer(
  config: RemoteImConfig,
  toUserId: string
): RemoteImPermissionResult<RemoteImManualSendDeniedReason> {
  if (localRole(config) === 'slave') {
    return { ok: false, reason: 'slave-cannot-initiate', peerRole: null }
  }
  const peerRelation = getRemoteImPeerRelation(config, toUserId)
  if (!peerRelation) return { ok: false, reason: 'peer-not-allowed', peerRole: null }
  return {
    ok: true,
    peerRole: peerRelation === 'friend' ? 'master' : peerRelation
  }
}

export function resolveDefaultRemoteImPeerUserId(config: RemoteImConfig): string | null {
  if (localRole(config) === 'slave') return null
  return masterUserIds(config)[0] ?? slaveUserIds(config)[0] ?? null
}
