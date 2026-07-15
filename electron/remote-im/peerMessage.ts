import type { CreateRemoteImMessageInput } from './messageStore.js'
import type { RemoteImConfig, RemoteImFileAttachment, RemoteImImageAttachment } from './types.js'
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

export function createPeerOutgoingImageMessageInput(input: {
  projectId: string
  config: RemoteImConfig
  toUserId: string
  attachment: RemoteImImageAttachment
  now: number
}): CreateRemoteImMessageInput {
  const fileName = input.attachment.fileName?.trim()
  return {
    projectId: input.projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: input.config.desktopUserId,
    toUserId: input.toUserId,
    role: 'remote-user',
    direction: 'outgoing',
    content: fileName ? `[图片消息] ${fileName}` : '[图片消息]',
    kind: 'image',
    attachment: input.attachment,
    status: 'streaming',
    createdAt: input.now,
    sentToImAt: null
  }
}

export function createPeerOutgoingFileMessageInput(input: {
  projectId: string
  config: RemoteImConfig
  toUserId: string
  attachment: RemoteImFileAttachment
  now: number
}): CreateRemoteImMessageInput {
  const fileName = input.attachment.fileName?.trim()
  return {
    projectId: input.projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: input.config.desktopUserId,
    toUserId: input.toUserId,
    role: 'remote-user',
    direction: 'outgoing',
    content: fileName ? `[文件消息] ${fileName}` : '[文件消息]',
    kind: 'file',
    attachment: input.attachment,
    status: 'streaming',
    createdAt: input.now,
    sentToImAt: null
  }
}
