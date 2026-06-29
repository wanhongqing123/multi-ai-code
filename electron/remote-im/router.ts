import type {
  RemoteImConfig,
  RemoteImIncomingTextMessage,
  RemoteImMessage
} from './types.js'
import {
  isRemoteImOperationFinishedText,
  parseRemoteImAicliOutputText
} from './outputForwarding.js'
import { buildRemoteImAicliDisplayText, buildRemoteImAicliPrompt } from './replyProtocol.js'
import { canRouteRemoteImTaskFrom, getRemoteImPeerRelation } from './rolePermissions.js'

export interface RemoteImSessionInfo {
  sessionId: string
  targetRepo: string
}

export interface RemoteImRouterStore {
  create(input: Omit<RemoteImMessage, 'id'>): RemoteImMessage
  updateStatus(id: number, patch: Partial<RemoteImMessage>): RemoteImMessage | null | undefined
}

export interface RemoteImRouterDeps {
  getConfig(projectId: string): RemoteImConfig
  resolveSession(projectId: string): RemoteImSessionInfo | null
  sendUser(
    sessionId: string,
    text: string,
    options?: { displayText?: string }
  ): Promise<{ ok: boolean; error?: string }>
  sendImText(projectId: string, toUserId: string, text: string): Promise<{ ok: boolean; error?: string }>
  store: RemoteImRouterStore
  now?: () => number
}

export interface RemoteImRouteResult {
  ok: boolean
  error?: string
  aicliSessionId?: string
}

const REMOTE_IM_SYSTEM_TEXTS = new Set([
  '没有远程控制权限。',
  '奴隶节点不能主动发起任务。',
  '奴隶节点不能互相通信。',
  '消息为空，未发送给 AICLI。',
  '当前没有运行中的 AICLI。',
  '已发送给当前 AICLI，开始处理。'
])

function isLikelyNestedRemoteImOutput(text: string): boolean {
  return text.includes('[来自远程 IM：') || text.includes('[来自远程IM：')
}

function isRemoteImSystemText(text: string): boolean {
  return (
    REMOTE_IM_SYSTEM_TEXTS.has(text) ||
    text.startsWith('发送给 AICLI 失败：') ||
    isRemoteImOperationFinishedText(text)
  )
}

function createIncomingRecord(
  message: RemoteImIncomingTextMessage,
  status: RemoteImMessage['status'],
  error: string | null,
  now: number,
  role: RemoteImMessage['role'] = 'remote-user'
): Omit<RemoteImMessage, 'id'> {
  return {
    projectId: message.projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: message.remoteMessageId ?? null,
    fromUserId: message.fromUserId,
    toUserId: message.toUserId ?? null,
    role,
    direction: 'incoming',
    content: message.text,
    status,
    error,
    createdAt: message.createdAt ?? now,
    sentToAicliAt: null,
    sentToImAt: null
  }
}

function createSystemRecord(
  projectId: string,
  toUserId: string,
  content: string,
  status: RemoteImMessage['status'],
  error: string | null,
  now: number
): Omit<RemoteImMessage, 'id'> {
  return {
    projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: null,
    toUserId,
    role: 'system',
    direction: 'outgoing',
    content,
    status,
    error,
    createdAt: now,
    sentToAicliAt: null,
    sentToImAt: status === 'sent-to-im' ? now : null
  }
}

async function sendSystemText(
  deps: RemoteImRouterDeps,
  projectId: string,
  toUserId: string,
  text: string
): Promise<void> {
  const now = deps.now?.() ?? Date.now()
  const result = await deps.sendImText(projectId, toUserId, text)
  deps.store.create(
    createSystemRecord(
      projectId,
      toUserId,
      text,
      result.ok ? 'sent-to-im' : 'failed',
      result.ok ? null : result.error ?? 'failed to send IM message',
      now
    )
  )
}

export function createRemoteImRouter(deps: RemoteImRouterDeps) {
  async function handleIncomingText(
    message: RemoteImIncomingTextMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const text = message.text.trim()

    if (!config.enabled) {
      const record = deps.store.create(createIncomingRecord(message, 'rejected', 'remote IM disabled', now))
      deps.store.updateStatus(record.id, { status: 'rejected', error: 'remote IM disabled' })
      return { ok: false, error: 'remote IM disabled' }
    }

    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    if (!peerRelation || (config.desktopRole === 'slave' && peerRelation === 'slave')) {
      const isSlaveToSlave = peerRelation === 'slave'
      deps.store.create(
        createIncomingRecord(
          message,
          'rejected',
          isSlaveToSlave ? 'slave-to-slave blocked' : 'sender not allowed',
          now
        )
      )
      return {
        ok: false,
        error: isSlaveToSlave
          ? 'slave nodes cannot route tasks to each other'
          : `sender ${fromUserId} is not allowed`
      }
    }

    if (isRemoteImSystemText(text)) {
      deps.store.create(createIncomingRecord(message, 'received', null, now))
      return { ok: true }
    }

    const remoteAicliOutput = parseRemoteImAicliOutputText(text)
    if (remoteAicliOutput !== null) {
      deps.store.create(
        createIncomingRecord(
          {
            ...message,
            text: remoteAicliOutput
          },
          'received',
          null,
          now,
          'aicli'
        )
      )
      return { ok: true }
    }

    if (isLikelyNestedRemoteImOutput(text)) {
      deps.store.create(createIncomingRecord(message, 'received', null, now, 'aicli'))
      return { ok: true }
    }

    if (peerRelation === 'friend') {
      deps.store.create(createIncomingRecord(message, 'received', null, now))
      return { ok: true }
    }

    if (!text) {
      deps.store.create(createIncomingRecord(message, 'rejected', 'empty message', now))
      await sendSystemText(deps, message.projectId, fromUserId, '消息为空，未发送给 AICLI。')
      return { ok: false, error: 'empty message' }
    }

    const routePermission = canRouteRemoteImTaskFrom(config, fromUserId)
    if (!routePermission.ok) {
      deps.store.create(
        createIncomingRecord(message, 'rejected', 'slave cannot initiate task', now)
      )
      await sendSystemText(deps, message.projectId, fromUserId, '奴隶节点不能主动发起任务。')
      return { ok: false, error: 'slave nodes cannot initiate tasks' }
    }

    const session = deps.resolveSession(message.projectId)
    const incoming = deps.store.create(createIncomingRecord(message, 'received', null, now))
    if (!session) {
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error: 'No running AICLI session'
      })
      await sendSystemText(deps, message.projectId, fromUserId, '当前没有运行中的 AICLI。')
      return { ok: false, error: 'No running AICLI session' }
    }

    const wrapped = buildRemoteImAicliPrompt({ fromUserId, text })
    const displayText = buildRemoteImAicliDisplayText({ fromUserId, text })
    const sendResult = await deps.sendUser(session.sessionId, wrapped, { displayText })
    if (!sendResult.ok) {
      const error = sendResult.error ?? 'failed to send message to AICLI'
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error
      })
      await sendSystemText(deps, message.projectId, fromUserId, `发送给 AICLI 失败：${error}`)
      return { ok: false, error }
    }

    deps.store.updateStatus(incoming.id, {
      sessionId: session.sessionId,
      status: 'sent-to-aicli',
      sentToAicliAt: now,
      error: null
    })
    await sendSystemText(deps, message.projectId, fromUserId, '已发送给当前 AICLI，开始处理。')
    return { ok: true, aicliSessionId: session.sessionId }
  }

  return {
    handleIncomingText
  }
}
