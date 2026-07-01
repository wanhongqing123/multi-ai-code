import type {
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
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
  transcribeAudio?: (
    message: RemoteImIncomingAudioMessage
  ) => Promise<{ ok: true; text: string } | { ok: false; error: string }>
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

function createIncomingAudioRecord(
  message: RemoteImIncomingAudioMessage,
  content: string,
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
    content,
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

function formatRemoteImAudioPlaceholder(message: RemoteImIncomingAudioMessage): string {
  const duration = message.durationSeconds
  return typeof duration === 'number' && Number.isFinite(duration) && duration > 0
    ? `[语音消息 ${Math.round(duration)}s]`
    : '[语音消息]'
}

export function createRemoteImRouter(deps: RemoteImRouterDeps) {
  async function routeTaskTextToAicli(input: {
    message: RemoteImIncomingTextMessage
    fromUserId: string
    text: string
    recordText: string
    now: number
  }): Promise<RemoteImRouteResult> {
    const routePermission = canRouteRemoteImTaskFrom(
      deps.getConfig(input.message.projectId),
      input.fromUserId
    )
    if (!routePermission.ok) {
      deps.store.create(
        createIncomingRecord(
          { ...input.message, text: input.recordText },
          'rejected',
          'sender not allowed',
          input.now
        )
      )
      return { ok: false, error: `sender ${input.fromUserId} is not allowed` }
    }

    const session = deps.resolveSession(input.message.projectId)
    const incoming = deps.store.create(
      createIncomingRecord(
        { ...input.message, text: input.recordText },
        'received',
        null,
        input.now
      )
    )
    if (!session) {
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error: 'No running AICLI session'
      })
      await sendSystemText(deps, input.message.projectId, input.fromUserId, '当前没有运行中的 AICLI。')
      return { ok: false, error: 'No running AICLI session' }
    }

    const wrapped = buildRemoteImAicliPrompt({ fromUserId: input.fromUserId, text: input.text })
    const displayText = buildRemoteImAicliDisplayText({
      fromUserId: input.fromUserId,
      text: input.text
    })
    const sendResult = await deps.sendUser(session.sessionId, wrapped, { displayText })
    if (!sendResult.ok) {
      const error = sendResult.error ?? 'failed to send message to AICLI'
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error
      })
      await sendSystemText(deps, input.message.projectId, input.fromUserId, `发送给 AICLI 失败：${error}`)
      return { ok: false, error }
    }

    deps.store.updateStatus(incoming.id, {
      sessionId: session.sessionId,
      status: 'sent-to-aicli',
      sentToAicliAt: input.now,
      error: null
    })
    await sendSystemText(deps, input.message.projectId, input.fromUserId, '已发送给当前 AICLI，开始处理。')
    return { ok: true, aicliSessionId: session.sessionId }
  }

  async function handleIncomingText(
    message: RemoteImIncomingTextMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const text = message.text.trim()

    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    if (!peerRelation) {
      deps.store.create(
        createIncomingRecord(
          message,
          'rejected',
          'sender not allowed',
          now
        )
      )
      return {
        ok: false,
        error: `sender ${fromUserId} is not allowed`
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

    if (!text) {
      deps.store.create(createIncomingRecord(message, 'rejected', 'empty message', now))
      await sendSystemText(deps, message.projectId, fromUserId, '消息为空，未发送给 AICLI。')
      return { ok: false, error: 'empty message' }
    }

    return routeTaskTextToAicli({
      message,
      fromUserId,
      text,
      recordText: text,
      now
    })
  }

  async function handleIncomingAudio(
    message: RemoteImIncomingAudioMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const placeholder = formatRemoteImAudioPlaceholder(message)

    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    if (!peerRelation) {
      deps.store.create(
        createIncomingAudioRecord(message, placeholder, 'rejected', 'sender not allowed', now)
      )
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!deps.transcribeAudio) {
      const error = '本地 Whisper 转写模块未初始化，请重启桌面端或重新构建主进程'
      deps.store.create(createIncomingAudioRecord(message, placeholder, 'failed', error, now))
      await sendSystemText(deps, message.projectId, fromUserId, `语音转文字失败：${error}`)
      return { ok: false, error }
    }

    const transcription = await deps.transcribeAudio(message)
    if (!transcription.ok || !transcription.text.trim()) {
      const error = transcription.ok ? '语音转文字结果为空' : transcription.error
      deps.store.create(createIncomingAudioRecord(message, placeholder, 'failed', error, now))
      await sendSystemText(deps, message.projectId, fromUserId, `语音转文字失败：${error}`)
      return { ok: false, error }
    }

    const transcriptText = `[语音转文字]\n${transcription.text.trim()}`
    return routeTaskTextToAicli({
      message: {
        projectId: message.projectId,
        remoteMessageId: message.remoteMessageId,
        fromUserId,
        toUserId: message.toUserId,
        text: transcriptText,
        createdAt: message.createdAt
      },
      fromUserId,
      text: transcriptText,
      recordText: `${placeholder}\n${transcriptText}`,
      now
    })
  }

  return {
    handleIncomingText,
    handleIncomingAudio
  }
}
