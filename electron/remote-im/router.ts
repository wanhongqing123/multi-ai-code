import type {
  RemoteImConfig,
  RemoteImIncomingTextMessage,
  RemoteImMessage
} from './types.js'

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
  sendUser(sessionId: string, text: string): Promise<{ ok: boolean; error?: string }>
  sendImText(projectId: string, toUserId: string, text: string): Promise<{ ok: boolean; error?: string }>
  store: RemoteImRouterStore
  now?: () => number
}

export interface RemoteImRouteResult {
  ok: boolean
  error?: string
}

function createIncomingRecord(
  message: RemoteImIncomingTextMessage,
  status: RemoteImMessage['status'],
  error: string | null,
  now: number
): Omit<RemoteImMessage, 'id'> {
  return {
    projectId: message.projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: message.remoteMessageId ?? null,
    fromUserId: message.fromUserId,
    toUserId: message.toUserId ?? null,
    role: 'remote-user',
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

    if (!config.allowedUserIds.includes(fromUserId)) {
      deps.store.create(createIncomingRecord(message, 'rejected', 'sender not allowed', now))
      await sendSystemText(deps, message.projectId, fromUserId, '没有远程控制权限。')
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!text) {
      deps.store.create(createIncomingRecord(message, 'rejected', 'empty message', now))
      await sendSystemText(deps, message.projectId, fromUserId, '消息为空，未发送给 AICLI。')
      return { ok: false, error: 'empty message' }
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

    const wrapped = `[来自远程 IM：${fromUserId}]\n${text}`
    const sendResult = await deps.sendUser(session.sessionId, wrapped)
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
    return { ok: true }
  }

  return {
    handleIncomingText
  }
}
