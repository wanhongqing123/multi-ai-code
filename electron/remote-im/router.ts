import type {
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingFileMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingTextMessage,
  RemoteImFileAttachment,
  RemoteImImageAttachment,
  RemoteImMessage
} from './types.js'
import {
  isRemoteImOperationFinishedText,
  parseRemoteImAicliOutputText
} from './outputForwarding.js'
import {
  buildRemoteImAicliDisplayText,
  buildRemoteImAicliPrompt,
  createRemoteImReplyId
} from './replyProtocol.js'
import { canRouteRemoteImTaskFrom, getRemoteImPeerRelation } from './rolePermissions.js'
import {
  formatRemoteImControlCommandHelp,
  parseRemoteImControlCommand,
  type RemoteImControlCommandName
} from './controlCommands.js'

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
  sendImText(
    projectId: string,
    toUserId: string,
    text: string,
    options?: { messageId?: number }
  ): Promise<{ ok: boolean; error?: string }>
  transcribeAudio?: (
    message: RemoteImIncomingAudioMessage
  ) => Promise<{ ok: true; text: string } | { ok: false; error: string }>
  cacheImage?: (
    message: RemoteImIncomingImageMessage
  ) => Promise<
    | { ok: true; attachment: RemoteImImageAttachment }
    | { ok: false; error: string; attachment?: RemoteImImageAttachment | null }
  >
  cacheFile?: (
    message: RemoteImIncomingFileMessage
  ) => Promise<
    | { ok: true; attachment: RemoteImFileAttachment }
    | { ok: false; error: string; attachment?: RemoteImFileAttachment | null }
  >
  createReplyId?: () => string
  handleControlCommand?: (input: {
    projectId: string
    fromUserId: string
    command: RemoteImControlCommandName
    args: string
    raw: string
  }) => Promise<{ ok: boolean; text: string }>
  store: RemoteImRouterStore
  now?: () => number
}

export interface RemoteImRouteResult {
  ok: boolean
  error?: string
  aicliSessionId?: string
  replyId?: string
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

function formatUnknownControlCommand(commandText: string): string {
  return [
    `不支持的 IM 控制命令：${commandText}`,
    '',
    formatRemoteImControlCommandHelp()
  ].join('\n')
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
    kind: 'text',
    attachment: null,
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
    kind: 'text',
    attachment: null,
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
    kind: 'text',
    attachment: null,
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
  const outgoing = deps.store.create(
    createSystemRecord(projectId, toUserId, text, 'streaming', null, now)
  )
  const result = await deps.sendImText(projectId, toUserId, text, {
    messageId: outgoing.id
  })
  if (!result.ok) {
    deps.store.updateStatus(outgoing.id, {
      status: 'failed',
      error: result.error ?? 'failed to send IM message'
    })
  }
}

function formatRemoteImAudioPlaceholder(message: RemoteImIncomingAudioMessage): string {
  const duration = message.durationSeconds
  return typeof duration === 'number' && Number.isFinite(duration) && duration > 0
    ? `[语音消息 ${Math.round(duration)}s]`
    : '[语音消息]'
}

function normalizeRemoteImString(value: string | null | undefined): string | null {
  const cleanValue = value?.trim()
  return cleanValue ? cleanValue : null
}

function normalizeRemoteImNumber(value: number | null | undefined): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function formatRemoteImImagePlaceholder(
  message: RemoteImIncomingImageMessage,
  attachment?: RemoteImImageAttachment | null
): string {
  const fileName = normalizeRemoteImString(attachment?.fileName) ?? normalizeRemoteImString(message.fileName)
  return fileName ? `[图片消息] ${fileName}` : '[图片消息]'
}

function createImageAttachmentFromIncoming(
  message: RemoteImIncomingImageMessage,
  patch: Partial<RemoteImImageAttachment> = {}
): RemoteImImageAttachment {
  return {
    type: 'image',
    localPath: patch.localPath ?? null,
    remoteUrl: patch.remoteUrl ?? normalizeRemoteImString(message.imageUrl),
    thumbnailUrl: patch.thumbnailUrl ?? normalizeRemoteImString(message.thumbnailUrl),
    width: patch.width ?? normalizeRemoteImNumber(message.width),
    height: patch.height ?? normalizeRemoteImNumber(message.height),
    sizeBytes: patch.sizeBytes ?? normalizeRemoteImNumber(message.sizeBytes),
    fileName: patch.fileName ?? normalizeRemoteImString(message.fileName),
    mimeType: patch.mimeType ?? normalizeRemoteImString(message.mimeType),
    sdkImageId: patch.sdkImageId ?? normalizeRemoteImString(message.uuid)
  }
}

function createIncomingImageRecord(
  message: RemoteImIncomingImageMessage,
  attachment: RemoteImImageAttachment | null,
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
    content: formatRemoteImImagePlaceholder(message, attachment),
    kind: 'image',
    attachment,
    status,
    error,
    createdAt: message.createdAt ?? now,
    sentToAicliAt: null,
    sentToImAt: null
  }
}

function formatRemoteImFilePlaceholder(
  message: RemoteImIncomingFileMessage,
  attachment?: RemoteImFileAttachment | null
): string {
  const fileName = normalizeRemoteImString(attachment?.fileName) ?? normalizeRemoteImString(message.fileName)
  return fileName ? `[文件消息] ${fileName}` : '[文件消息]'
}

function createFileAttachmentFromIncoming(
  message: RemoteImIncomingFileMessage,
  patch: Partial<RemoteImFileAttachment> = {}
): RemoteImFileAttachment {
  return {
    type: 'file',
    localPath: patch.localPath ?? null,
    remoteUrl: patch.remoteUrl ?? normalizeRemoteImString(message.fileUrl),
    sizeBytes: patch.sizeBytes ?? normalizeRemoteImNumber(message.sizeBytes),
    fileName: patch.fileName ?? normalizeRemoteImString(message.fileName),
    mimeType: patch.mimeType ?? normalizeRemoteImString(message.mimeType),
    sdkFileId: patch.sdkFileId ?? normalizeRemoteImString(message.uuid)
  }
}

function createIncomingFileRecord(
  message: RemoteImIncomingFileMessage,
  attachment: RemoteImFileAttachment | null,
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
    content: formatRemoteImFilePlaceholder(message, attachment),
    kind: 'file',
    attachment,
    status,
    error,
    createdAt: message.createdAt ?? now,
    sentToAicliAt: null,
    sentToImAt: null
  }
}

function buildRemoteImImageTaskText(input: {
  fromUserId: string
  localPath: string
}): string {
  return [
    '[图片消息]',
    `来自: ${input.fromUserId}`,
    `本地路径: ${input.localPath}`,
    '请根据图片内容和上下文继续处理。'
  ].join('\n')
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

    const replyId = deps.createReplyId?.() ?? createRemoteImReplyId()
    const wrapped = buildRemoteImAicliPrompt({
      fromUserId: input.fromUserId,
      text: input.text,
      replyId
    })
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
    return { ok: true, aicliSessionId: session.sessionId, replyId }
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

    const controlCommand = parseRemoteImControlCommand(text)
    if (controlCommand.type === 'unknown-command') {
      const routePermission = canRouteRemoteImTaskFrom(config, fromUserId)
      if (!routePermission.ok) {
        deps.store.create(createIncomingRecord(message, 'rejected', 'sender not allowed', now))
        return { ok: false, error: `sender ${fromUserId} is not allowed` }
      }
      deps.store.create(createIncomingRecord(message, 'rejected', 'unsupported control command', now))
      await sendSystemText(
        deps,
        message.projectId,
        fromUserId,
        formatUnknownControlCommand(controlCommand.commandText)
      )
      return {
        ok: false,
        error: `unsupported remote IM control command: ${controlCommand.commandText}`
      }
    }

    if (controlCommand.type === 'command') {
      const routePermission = canRouteRemoteImTaskFrom(config, fromUserId)
      if (!routePermission.ok) {
        deps.store.create(createIncomingRecord(message, 'rejected', 'sender not allowed', now))
        return { ok: false, error: `sender ${fromUserId} is not allowed` }
      }
      deps.store.create(createIncomingRecord(message, 'received', null, now))
      const result = deps.handleControlCommand
        ? await deps.handleControlCommand({
            projectId: message.projectId,
            fromUserId,
            command: controlCommand.command,
            args: controlCommand.args,
            raw: controlCommand.raw
          })
        : {
            ok: false,
            text: '当前桌面端未接入 IM 控制命令。'
          }
      await sendSystemText(deps, message.projectId, fromUserId, result.text)
      return result.ok
        ? { ok: true }
        : { ok: false, error: `remote IM control command failed: ${controlCommand.command}` }
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

  async function handleIncomingImage(
    message: RemoteImIncomingImageMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    const fallbackAttachment = createImageAttachmentFromIncoming(message)

    if (!peerRelation) {
      deps.store.create(
        createIncomingImageRecord(message, fallbackAttachment, 'rejected', 'sender not allowed', now)
      )
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!deps.cacheImage) {
      const error = '图片下载模块未初始化'
      deps.store.create(createIncomingImageRecord(message, fallbackAttachment, 'failed', error, now))
      await sendSystemText(deps, message.projectId, fromUserId, `图片下载失败：${error}`)
      return { ok: false, error }
    }

    const cached = await deps.cacheImage(message)
    if (!cached.ok) {
      const error = cached.error || '图片下载失败'
      deps.store.create(
        createIncomingImageRecord(
          message,
          cached.attachment ?? fallbackAttachment,
          'failed',
          error,
          now
        )
      )
      await sendSystemText(deps, message.projectId, fromUserId, `图片下载失败：${error}`)
      return { ok: false, error }
    }

    const attachment = cached.attachment
    if (!attachment.localPath) {
      const error = '图片本地路径为空'
      deps.store.create(createIncomingImageRecord(message, attachment, 'failed', error, now))
      await sendSystemText(deps, message.projectId, fromUserId, `图片下载失败：${error}`)
      return { ok: false, error }
    }

    const session = deps.resolveSession(message.projectId)
    const incoming = deps.store.create(
      createIncomingImageRecord(message, attachment, 'received', null, now)
    )
    if (!session) {
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error: 'No running AICLI session'
      })
      await sendSystemText(deps, message.projectId, fromUserId, '当前没有运行中的 AICLI。')
      return { ok: false, error: 'No running AICLI session' }
    }

    const taskText = buildRemoteImImageTaskText({
      fromUserId,
      localPath: attachment.localPath
    })
    const replyId = deps.createReplyId?.() ?? createRemoteImReplyId()
    const wrapped = buildRemoteImAicliPrompt({ fromUserId, text: taskText, replyId })
    const displayText = buildRemoteImAicliDisplayText({ fromUserId, text: taskText })
    const sendResult = await deps.sendUser(session.sessionId, wrapped, { displayText })
    if (!sendResult.ok) {
      const error = sendResult.error ?? 'failed to send image message to AICLI'
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
    return { ok: true, aicliSessionId: session.sessionId, replyId }
  }

  async function handleIncomingFile(
    message: RemoteImIncomingFileMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    const fallbackAttachment = createFileAttachmentFromIncoming(message)

    if (!peerRelation) {
      deps.store.create(
        createIncomingFileRecord(message, fallbackAttachment, 'rejected', 'sender not allowed', now)
      )
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!deps.cacheFile) {
      const error = '文件下载模块未初始化'
      deps.store.create(createIncomingFileRecord(message, fallbackAttachment, 'failed', error, now))
      await sendSystemText(deps, message.projectId, fromUserId, `文件下载失败：${error}`)
      return { ok: false, error }
    }

    const cached = await deps.cacheFile(message)
    if (!cached.ok) {
      const error = cached.error || '文件下载失败'
      deps.store.create(
        createIncomingFileRecord(
          message,
          cached.attachment ?? fallbackAttachment,
          'failed',
          error,
          now
        )
      )
      await sendSystemText(deps, message.projectId, fromUserId, `文件下载失败：${error}`)
      return { ok: false, error }
    }

    deps.store.create(createIncomingFileRecord(message, cached.attachment, 'received', null, now))
    return { ok: true }
  }

  return {
    handleIncomingText,
    handleIncomingAudio,
    handleIncomingImage,
    handleIncomingFile
  }
}
