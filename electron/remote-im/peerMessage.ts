import type { CreateRemoteImMessageInput } from './messageStore.js'
import type { RemoteImConfig } from './types.js'
import {
  canManuallySendToRemoteImPeer,
  resolveDefaultRemoteImPeerUserId
} from './rolePermissions.js'

export function resolvePeerUserId(config: RemoteImConfig, requestedUserId?: string | null): string | null {
  const requested = requestedUserId?.trim()
  if (requested) {
    return canManuallySendToRemoteImPeer(config, requested).ok ? requested : null
  }
  return resolveDefaultRemoteImPeerUserId(config)
}

export function createPeerOutgoingMessageInput(input: {
  projectId: string
  config: RemoteImConfig
  toUserId: string
  text: string
  now: number
}): CreateRemoteImMessageInput {
  return {
    projectId: input.projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: input.config.desktopUserId,
    toUserId: input.toUserId,
    role: 'remote-user',
    direction: 'outgoing',
    content: input.text,
    status: 'streaming',
    createdAt: input.now,
    sentToImAt: null
  }
}
