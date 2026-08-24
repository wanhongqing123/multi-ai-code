import type { RemoteImConfig, RemoteImContactRelation } from './types.js'

// 主从角色早已不再区分，这里保留类型名只为不惊动调用方；取值恒为 'master'。
export type RemoteImPeerRole = 'master'
export type RemoteImPeerRelation = RemoteImContactRelation

export type RemoteImInboundTaskDeniedReason = 'sender-not-allowed'
export type RemoteImManualSendDeniedReason = 'peer-not-allowed'

export type RemoteImPermissionResult<TReason extends string> =
  | { ok: true; peerRole: RemoteImPeerRole }
  | { ok: false; reason: TReason; peerRole: RemoteImPeerRole | null }

function friendUserIds(config: RemoteImConfig): string[] {
  return Array.isArray(config.friendUserIds) ? config.friendUserIds : []
}

function trustedFriendUserIds(config: RemoteImConfig): string[] {
  return Array.from(new Set(friendUserIds(config)))
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
  return friendUserIds(config)[0] ?? null
}
