import { randomUUID } from 'node:crypto'
import type {
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingFileMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingTextMessage,
  RemoteImFileAttachment,
  RemoteImImageAttachment,
  RemoteImMessageOrigin,
  RemoteImMessage,
  RemoteImRoamedTextMessage
} from './types.js'
import type { RemoteImAicliOutputSourceKind } from './outputSanitizer.js'
import {
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
import type { AicliUserMessageAttachment } from '../aicli/structuredOutputBridge.js'

// Remote-desktop frames share the Tencent text transport, but they are an
// application protocol rather than chat/model input. Native MaiChat consumes
// the same prefix before storing chat history; keep Electron aligned so a
// program-generated signal can never be interpreted as machine collaboration.
const REMOTE_DESKTOP_SIGNAL_PREFIX = '\u2063\u200B[remote-desktop]'

function isRemoteDesktopSignalText(text: string): boolean {
  return text.startsWith(REMOTE_DESKTOP_SIGNAL_PREFIX)
}

export interface RemoteImSessionInfo {
  sessionId: string
  targetRepo: string
  sourceKind?: RemoteImAicliOutputSourceKind
}

export interface RemoteImRouterStore {
  create(input: Omit<RemoteImMessage, 'id'>): RemoteImMessage
  updateStatus(id: number, patch: Partial<RemoteImMessage>): RemoteImMessage | null | undefined
  // 漫游补拉用：按 provider + remoteMessageId 查已入库消息（去重与插入计数）。
  findByRemoteMessageId?(
    provider: RemoteImMessage['provider'],
    remoteMessageId: string
  ): RemoteImMessage | null
}

export interface RemoteImAicliOutputRoute {
  projectId: string
  toUserId: string
  sessionId: string
  replyId?: string
  taskId: string
  autoReplyToIm: boolean
  continuation?: boolean
}

export interface RemoteImRouterDeps {
  getConfig(projectId: string): RemoteImConfig
  resolveSession(projectId: string): RemoteImSessionInfo | null
  sendUser(
    sessionId: string,
    text: string,
    options?: {
      displayText?: string
      attachments?: AicliUserMessageAttachment[]
      inputOrigin?: 'remote-im' | 'remote-im-machine' | 'local'
      replyId?: string
      taskId?: string
    }
  ): Promise<{ ok: boolean; error?: string }>
  sendImText(
    projectId: string,
    toUserId: string,
    text: string,
    options?: { messageId?: number }
  ): Promise<{ ok: boolean; error?: string }>
  sendImFile?: (
    projectId: string,
    toUserId: string,
    localPath: string
  ) => Promise<{ ok: boolean; error?: string }>
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
  createTaskId?: (replyId: string) => string
  authorizeAicliOutputStart?: (
    route: RemoteImAicliOutputRoute
  ) => { ok: true } | { ok: false; error: string }
  onAicliOutputStart?: (route: RemoteImAicliOutputRoute) => void
  onAicliInputAccepted?: (route: RemoteImAicliOutputRoute) => void
  onAicliInputRejected?: (route: RemoteImAicliOutputRoute) => void
  onAicliMachineInputAccepted?: (sessionId: string) => void
  onAicliOutputCancel?: (route: RemoteImAicliOutputRoute) => void
  handleControlCommand?: (input: {
    projectId: string
    fromUserId: string
    command: RemoteImControlCommandName
    args: string
    raw: string
    replyId?: string
    taskId?: string
  }) => Promise<{ ok: boolean; text: string; attachmentPath?: string }>
  handleApprovalCommand?: (input: {
    projectId: string
    fromUserId: string
    text: string
  }) => Promise<{ handled: boolean; ok: boolean; text: string }>
  store: RemoteImRouterStore
  messagesChanged?: (projectId: string) => void
  now?: () => number
}

export interface RemoteImRouteResult {
  ok: boolean
  error?: string
  aicliSessionId?: string
  replyId?: string
}

function isIncomingAlreadySentToAicli(message: RemoteImMessage): boolean {
  return (
    message.direction === 'incoming' &&
    message.remoteMessageId !== null &&
    (message.status === 'sent-to-aicli' ||
      message.sentToAicliAt !== null ||
      // A received row with a bound session has already reserved a FIFO slot.
      // Treat a Tencent retransmission as the same queued input instead of
      // enqueueing the same machine message twice.
      (message.status === 'received' && message.sessionId !== null))
  )
}

function formatUnknownControlCommand(commandText: string): string {
  return [
    `不支持的 IM 控制命令：${commandText}`,
    '',
    formatRemoteImControlCommandHelp()
  ].join('\n')
}

function normalizeIncomingOrigin(origin: RemoteImMessageOrigin | undefined): RemoteImMessageOrigin {
  // All upgraded first-party senders attach an explicit origin. Unknown or
  // legacy traffic fails closed: it still reaches AICLI, but can never start an
  // automatic IM reply chain.
  return origin === 'human' ? 'human' : 'machine'
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
  deps.messagesChanged?.(projectId)
  const result = await deps.sendImText(projectId, toUserId, text, {
    messageId: outgoing.id
  })
  if (!result.ok) {
    deps.store.updateStatus(outgoing.id, {
      status: 'failed',
      error: result.error ?? 'failed to send IM message'
    })
    deps.messagesChanged?.(projectId)
  }
}

async function sendIncomingFailureIfHuman(
  deps: RemoteImRouterDeps,
  origin: RemoteImMessageOrigin,
  projectId: string,
  toUserId: string,
  text: string
): Promise<void> {
  // A machine-originated processing failure must stop locally. Automatically
  // replying with another machine error lets two unavailable/busy hosts bounce
  // failure receipts forever without either AICLI participating.
  if (origin !== 'human') return
  await sendSystemText(deps, projectId, toUserId, text)
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
  const base = fileName ? `[图片消息] ${fileName}` : '[图片消息]'
  const caption = normalizeRemoteImString(message.caption)
  return caption ? `${base}\n${caption}` : base
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
  caption?: string | null
}): string {
  const caption = input.caption?.trim()
  const lines = ['[图片消息]', `来自: ${input.fromUserId}`, `本地路径: ${input.localPath}`]
  if (caption) {
    // 图片与配文来自同一条消息，合并成一次输入：配文即用户随图发来的文字。
    lines.push(`配文: ${caption}`)
    lines.push('请结合配文与图片内容继续处理。')
  } else {
    lines.push('请根据图片内容和上下文继续处理。')
  }
  return lines.join('\n')
}

function inferImageMimeType(localPath: string): string {
  const lower = localPath.toLowerCase()
  if (lower.endsWith('.jpg') || lower.endsWith('.jpeg')) return 'image/jpeg'
  if (lower.endsWith('.webp')) return 'image/webp'
  if (lower.endsWith('.gif')) return 'image/gif'
  if (lower.endsWith('.heic')) return 'image/heic'
  return 'image/png'
}

function buildRemoteImFileTaskText(input: {
  fromUserId: string
  localPath: string
  fileName?: string | null
  sizeBytes?: number | null
  mimeType?: string | null
  caption?: string | null
}): string {
  const caption = input.caption?.trim()
  const lines = ['[文件消息]', `来自: ${input.fromUserId}`]
  const fileName = input.fileName?.trim()
  if (fileName) lines.push(`文件名: ${fileName}`)
  // 类型与大小先给出来：收到一个几十 MB 的二进制时，AICLI 该有机会先判断
  // 值不值得读，而不是闷头打开。
  if (input.mimeType?.trim()) lines.push(`类型: ${input.mimeType.trim()}`)
  if (typeof input.sizeBytes === 'number' && Number.isFinite(input.sizeBytes)) {
    lines.push(`大小: ${formatRemoteImFileSize(input.sizeBytes)}`)
  }
  lines.push(`本地路径: ${input.localPath}`)
  if (caption) {
    lines.push(`配文: ${caption}`)
    lines.push('请结合配文与文件内容继续处理。')
  } else {
    lines.push('请根据文件内容和上下文继续处理。')
  }
  return lines.join('\n')
}

function formatRemoteImFileSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

export function createRemoteImRouter(deps: RemoteImRouterDeps) {
  function usesSourceLevelRouting(session: RemoteImSessionInfo): boolean {
    return session.sourceKind === 'codex' || session.sourceKind === 'opencode'
  }

  function createOutputRoute(
    session: RemoteImSessionInfo,
    projectId: string,
    toUserId: string,
    autoReplyToIm: boolean
  ): RemoteImAicliOutputRoute {
    const replyId = deps.createReplyId?.() ?? createRemoteImReplyId()
    if (usesSourceLevelRouting(session)) {
      return {
        projectId,
        toUserId,
        sessionId: session.sessionId,
        replyId,
        taskId: `remote-im-route-${randomUUID()}`,
        autoReplyToIm
      }
    }
    return {
      projectId,
      toUserId,
      sessionId: session.sessionId,
      replyId,
      taskId: deps.createTaskId?.(replyId) ?? `remote-im-task-${replyId}`,
      autoReplyToIm
    }
  }

  async function sendUserWithOutputRoute(
    outputRoute: RemoteImAicliOutputRoute,
    text: string | (() => string),
    displayText: string,
    attachments?: AicliUserMessageAttachment[]
  ): Promise<Awaited<ReturnType<RemoteImRouterDeps['sendUser']>>> {
    const admission = deps.authorizeAicliOutputStart?.(outputRoute)
    if (admission && !admission.ok) return admission
    const resolvedText = typeof text === 'function' ? text() : text
    deps.onAicliOutputStart?.(outputRoute)
    try {
      const result = await deps.sendUser(outputRoute.sessionId, resolvedText, {
        displayText,
        inputOrigin: 'remote-im',
        ...(outputRoute.replyId ? { replyId: outputRoute.replyId } : {}),
        taskId: outputRoute.taskId,
        ...(attachments?.length ? { attachments } : {})
      })
      if (result.ok) {
        deps.onAicliInputAccepted?.(outputRoute)
      } else if (outputRoute.continuation) {
        deps.onAicliInputRejected?.(outputRoute)
      } else {
        deps.onAicliOutputCancel?.(outputRoute)
      }
      return result
    } catch (error) {
      if (outputRoute.continuation) deps.onAicliInputRejected?.(outputRoute)
      else deps.onAicliOutputCancel?.(outputRoute)
      throw error
    }
  }

  async function sendMachineInput(
    sessionId: string,
    text: string,
    displayText: string,
    attachments?: AicliUserMessageAttachment[]
  ): Promise<Awaited<ReturnType<RemoteImRouterDeps['sendUser']>>> {
    const result = await deps.sendUser(sessionId, text, {
      displayText,
      inputOrigin: 'remote-im-machine',
      ...(attachments?.length ? { attachments } : {})
    })
    if (result.ok) deps.onAicliMachineInputAccepted?.(sessionId)
    return result
  }

  async function routeTaskTextToAicli(input: {
    message: RemoteImIncomingTextMessage
    fromUserId: string
    text: string
    recordText: string
    now: number
    origin: RemoteImMessageOrigin
    recordRole?: RemoteImMessage['role']
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
        input.now,
        input.recordRole ?? (input.origin === 'machine' ? 'aicli' : 'remote-user')
      )
    )
    if (isIncomingAlreadySentToAicli(incoming)) {
      return {
        ok: true,
        ...(incoming.sessionId ? { aicliSessionId: incoming.sessionId } : {})
      }
    }
    if (!session) {
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error: 'No running AICLI session'
      })
      await sendIncomingFailureIfHuman(
        deps,
        input.origin,
        input.message.projectId,
        input.fromUserId,
        '当前没有运行中的 AICLI。'
      )
      return { ok: false, error: 'No running AICLI session' }
    }

    const outputRoute =
      input.origin === 'human'
        ? createOutputRoute(session, input.message.projectId, input.fromUserId, true)
        : null
    const buildPrompt = () =>
      buildRemoteImAicliPrompt(
        {
          fromUserId: input.fromUserId,
          text: input.text,
          replyId: outputRoute?.replyId
        },
        {
          includeReplyProtocol:
            input.origin === 'human' && !usesSourceLevelRouting(session)
        }
      )
    const displayText = buildRemoteImAicliDisplayText({
      fromUserId: input.fromUserId,
      text: input.text
    })
    // Reserve the incoming row synchronously before the first sendUser await.
    // Tencent may redeliver the same remoteMessageId while the provider is
    // accepting the prompt; the bound session makes that retransmission a
    // no-op instead of a second model input.
    deps.store.updateStatus(incoming.id, { sessionId: session.sessionId, error: null })
    const sendResult = outputRoute
      ? await sendUserWithOutputRoute(outputRoute, buildPrompt, displayText)
      : await sendMachineInput(session.sessionId, buildPrompt(), displayText)
    if (!sendResult.ok) {
      const error = sendResult.error ?? 'failed to send message to AICLI'
      deps.store.updateStatus(incoming.id, { status: 'failed', error })
      await sendIncomingFailureIfHuman(
        deps,
        input.origin,
        input.message.projectId,
        input.fromUserId,
        `发送给 AICLI 失败：${error}`
      )
      return { ok: false, error }
    }

    deps.store.updateStatus(incoming.id, {
      sessionId: session.sessionId,
      status: 'sent-to-aicli',
      sentToAicliAt: deps.now?.() ?? Date.now(),
      error: null
    })
    return {
      ok: true,
      aicliSessionId: session.sessionId,
      ...(outputRoute?.replyId ? { replyId: outputRoute.replyId } : {})
    }
  }

  async function handleIncomingText(
    message: RemoteImIncomingTextMessage
  ): Promise<RemoteImRouteResult> {
    if (isRemoteDesktopSignalText(message.text)) {
      return { ok: true }
    }
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const rawText = message.text.trim()
    const legacyAicliOutput = parseRemoteImAicliOutputText(rawText)
    const origin = legacyAicliOutput === null
      ? normalizeIncomingOrigin(message.origin)
      : 'machine'
    const text = (legacyAicliOutput ?? rawText).trim()
    const recordRole: RemoteImMessage['role'] = origin === 'machine' ? 'aicli' : 'remote-user'

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

    // Approval capabilities are consumed before general slash-command parsing
    // and before ordinary task routing, so an approval response can never be
    // forwarded to the model as user input.
    if (deps.handleApprovalCommand) {
      const approval = await deps.handleApprovalCommand({
        projectId: message.projectId,
        fromUserId,
        text
      })
      if (approval.handled) {
        deps.store.create(
          createIncomingRecord(
            { ...message, text },
            approval.ok ? 'received' : 'rejected',
            approval.ok ? null : 'approval command rejected',
            now,
            recordRole
          )
        )
        await sendSystemText(deps, message.projectId, fromUserId, approval.text)
        return approval.ok
          ? { ok: true }
          : { ok: false, error: 'remote IM approval command rejected' }
      }
    }

    // Only a human UI message controls the local host. Machine messages are
    // collaboration input and must reach AICLI verbatim; approval capabilities
    // above remain the one intentional exception.
    const controlCommand = origin === 'human'
      ? parseRemoteImControlCommand(text)
      : { type: 'not-command' as const }
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
      const incoming = deps.store.create(createIncomingRecord(message, 'received', null, now))
      const session = controlCommand.command === 'btw' ? deps.resolveSession(message.projectId) : null
      const replyId =
        controlCommand.command === 'btw'
          ? deps.createReplyId?.() ?? createRemoteImReplyId()
          : undefined
      const taskId = replyId
        ? deps.createTaskId?.(replyId) ?? `remote-im-task-${replyId}`
        : undefined
      const outputRoute =
        session && replyId && taskId
          ? {
              projectId: message.projectId,
              toUserId: fromUserId,
              sessionId: session.sessionId,
              replyId,
              taskId,
              autoReplyToIm: true
            }
          : undefined
      if (outputRoute) {
        // Keep `/btw` behind the same conservative admission gate. Until its
        // side-thread protocol carries the same taskId authority all the way
        // through, allowing it beside another remote task could misroute an
        // approval even when the requester happens to be the same person.
        const admission = deps.authorizeAicliOutputStart?.(outputRoute)
        if (admission && !admission.ok) {
          deps.store.updateStatus(incoming.id, {
            status: 'failed',
            error: admission.error
          })
          await sendSystemText(
            deps,
            message.projectId,
            fromUserId,
            `发送给 AICLI 失败：${admission.error}`
          )
          return { ok: false, error: admission.error }
        }
        deps.onAicliOutputStart?.(outputRoute)
      }
      let result: Awaited<ReturnType<NonNullable<RemoteImRouterDeps['handleControlCommand']>>>
      try {
        result = deps.handleControlCommand
          ? await deps.handleControlCommand({
              projectId: message.projectId,
              fromUserId,
              command: controlCommand.command,
              args: controlCommand.args,
              raw: controlCommand.raw,
              ...(replyId ? { replyId } : {}),
              ...(taskId ? { taskId } : {})
            })
          : {
              ok: false,
              text: '当前桌面端未接入 IM 控制命令。'
            }
      } catch (error) {
        if (outputRoute) deps.onAicliOutputCancel?.(outputRoute)
        throw error
      }
      if (!result.ok && outputRoute) deps.onAicliOutputCancel?.(outputRoute)
      await sendSystemText(deps, message.projectId, fromUserId, result.text)
      if (result.ok && result.attachmentPath) {
        if (!deps.sendImFile) {
          await sendSystemText(deps, message.projectId, fromUserId, 'Diff 附件发送模块未初始化。')
          return { ok: false, error: 'remote IM file sender is not available' }
        }
        const fileResult = await deps.sendImFile(
          message.projectId,
          fromUserId,
          result.attachmentPath
        )
        if (!fileResult.ok) {
          const error = fileResult.error ?? 'failed to send Diff attachment'
          await sendSystemText(deps, message.projectId, fromUserId, `Diff 附件发送失败：${error}`)
          return { ok: false, error }
        }
      }
      if (result.ok && controlCommand.command === 'btw' && session && replyId) {
        return { ok: true, aicliSessionId: session.sessionId, replyId }
      }
      return result.ok
        ? { ok: true }
        : { ok: false, error: `remote IM control command failed: ${controlCommand.command}` }
    }

    if (!text) {
      deps.store.create(createIncomingRecord(message, 'rejected', 'empty message', now))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        '消息为空，未发送给 AICLI。'
      )
      return { ok: false, error: 'empty message' }
    }

    return routeTaskTextToAicli({
      message: { ...message, text },
      fromUserId,
      text,
      recordText: text,
      now,
      origin,
      recordRole
    })
  }

  async function handleIncomingAudio(
    message: RemoteImIncomingAudioMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const placeholder = formatRemoteImAudioPlaceholder(message)
    const origin = normalizeIncomingOrigin(message.origin)
    const recordRole: RemoteImMessage['role'] = origin === 'machine' ? 'aicli' : 'remote-user'

    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    if (!peerRelation) {
      deps.store.create(
        createIncomingAudioRecord(message, placeholder, 'rejected', 'sender not allowed', now, recordRole)
      )
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!deps.transcribeAudio) {
      const error = '本地 Whisper 转写模块未初始化，请重启桌面端或重新构建主进程'
      deps.store.create(createIncomingAudioRecord(message, placeholder, 'failed', error, now, recordRole))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `语音转文字失败：${error}`
      )
      return { ok: false, error }
    }

    const transcription = await deps.transcribeAudio(message)
    if (!transcription.ok || !transcription.text.trim()) {
      const error = transcription.ok ? '语音转文字结果为空' : transcription.error
      deps.store.create(createIncomingAudioRecord(message, placeholder, 'failed', error, now, recordRole))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `语音转文字失败：${error}`
      )
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
        origin,
        createdAt: message.createdAt
      },
      fromUserId,
      text: transcriptText,
      recordText: `${placeholder}\n${transcriptText}`,
      now,
      origin,
      recordRole
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
    const origin = normalizeIncomingOrigin(message.origin)
    const recordRole: RemoteImMessage['role'] = origin === 'machine' ? 'aicli' : 'remote-user'

    if (!peerRelation) {
      deps.store.create(
        createIncomingImageRecord(message, fallbackAttachment, 'rejected', 'sender not allowed', now, recordRole)
      )
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!deps.cacheImage) {
      const error = '图片下载模块未初始化'
      deps.store.create(createIncomingImageRecord(message, fallbackAttachment, 'failed', error, now, recordRole))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `图片下载失败：${error}`
      )
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
          now,
          recordRole
        )
      )
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `图片下载失败：${error}`
      )
      return { ok: false, error }
    }

    const attachment = cached.attachment
    if (!attachment.localPath) {
      const error = '图片本地路径为空'
      deps.store.create(createIncomingImageRecord(message, attachment, 'failed', error, now, recordRole))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `图片下载失败：${error}`
      )
      return { ok: false, error }
    }

    const session = deps.resolveSession(message.projectId)
    const incoming = deps.store.create(
      createIncomingImageRecord(message, attachment, 'received', null, now, recordRole)
    )
    if (isIncomingAlreadySentToAicli(incoming)) {
      return {
        ok: true,
        ...(incoming.sessionId ? { aicliSessionId: incoming.sessionId } : {})
      }
    }
    if (!session) {
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error: 'No running AICLI session'
      })
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        '当前没有运行中的 AICLI。'
      )
      return { ok: false, error: 'No running AICLI session' }
    }

    const taskText = buildRemoteImImageTaskText({
      fromUserId,
      localPath: attachment.localPath,
      caption: message.caption ?? null
    })
    const outputRoute =
      origin === 'human' ? createOutputRoute(session, message.projectId, fromUserId, true) : null
    const buildPrompt = () =>
      buildRemoteImAicliPrompt(
        { fromUserId, text: taskText, replyId: outputRoute?.replyId },
        { includeReplyProtocol: origin === 'human' && !usesSourceLevelRouting(session) }
      )
    const displayText = buildRemoteImAicliDisplayText({
      fromUserId,
      text: taskText
    })
    deps.store.updateStatus(incoming.id, { sessionId: session.sessionId, error: null })
    const attachments: AicliUserMessageAttachment[] = [
      {
        type: 'image',
        localPath: attachment.localPath!,
        mimeType: attachment.mimeType?.startsWith('image/')
          ? attachment.mimeType
          : inferImageMimeType(attachment.localPath!),
        ...(attachment.fileName ? { fileName: attachment.fileName } : {})
      }
    ]
    const sendResult = outputRoute
      ? await sendUserWithOutputRoute(outputRoute, buildPrompt, displayText, attachments)
      : await sendMachineInput(session.sessionId, buildPrompt(), displayText, attachments)
    if (!sendResult.ok) {
      const error = sendResult.error ?? 'failed to send image message to AICLI'
      deps.store.updateStatus(incoming.id, { status: 'failed', error })
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `发送给 AICLI 失败：${error}`
      )
      return { ok: false, error }
    }
    deps.store.updateStatus(incoming.id, {
      sessionId: session.sessionId,
      status: 'sent-to-aicli',
      sentToAicliAt: deps.now?.() ?? Date.now(),
      error: null
    })
    return {
      ok: true,
      aicliSessionId: session.sessionId,
      ...(outputRoute?.replyId ? { replyId: outputRoute.replyId } : {})
    }
  }

  async function handleIncomingFile(
    message: RemoteImIncomingFileMessage
  ): Promise<RemoteImRouteResult> {
    const config = deps.getConfig(message.projectId)
    const now = deps.now?.() ?? Date.now()
    const fromUserId = message.fromUserId.trim()
    const peerRelation = getRemoteImPeerRelation(config, fromUserId)
    const fallbackAttachment = createFileAttachmentFromIncoming(message)
    const origin = normalizeIncomingOrigin(message.origin)
    const recordRole: RemoteImMessage['role'] = origin === 'machine' ? 'aicli' : 'remote-user'

    if (!peerRelation) {
      deps.store.create(
        createIncomingFileRecord(message, fallbackAttachment, 'rejected', 'sender not allowed', now, recordRole)
      )
      return { ok: false, error: `sender ${fromUserId} is not allowed` }
    }

    if (!deps.cacheFile) {
      const error = '文件下载模块未初始化'
      deps.store.create(createIncomingFileRecord(message, fallbackAttachment, 'failed', error, now, recordRole))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `文件下载失败：${error}`
      )
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
          now,
          recordRole
        )
      )
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `文件下载失败：${error}`
      )
      return { ok: false, error }
    }

    const attachment = cached.attachment
    if (!attachment.localPath) {
      const error = '文件本地路径为空'
      deps.store.create(createIncomingFileRecord(message, attachment, 'failed', error, now, recordRole))
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `文件下载失败：${error}`
      )
      return { ok: false, error }
    }

    // 以前到这里就结束了：只入库供界面显示，**从不转给 AICLI**。于是用户发来
    // 的文件在对话里毫无反应，看起来像没收到。现在与图片走同一条路。
    const session = deps.resolveSession(message.projectId)
    const incoming = deps.store.create(
      createIncomingFileRecord(message, attachment, 'received', null, now, recordRole)
    )
    if (isIncomingAlreadySentToAicli(incoming)) {
      return {
        ok: true,
        ...(incoming.sessionId ? { aicliSessionId: incoming.sessionId } : {})
      }
    }
    if (!session) {
      deps.store.updateStatus(incoming.id, {
        status: 'failed',
        error: 'No running AICLI session'
      })
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        '当前没有运行中的 AICLI。'
      )
      return { ok: false, error: 'No running AICLI session' }
    }

    const taskText = buildRemoteImFileTaskText({
      fromUserId,
      localPath: attachment.localPath,
      fileName: attachment.fileName ?? message.fileName ?? null,
      sizeBytes: attachment.sizeBytes ?? null,
      mimeType: attachment.mimeType ?? null,
      caption: message.caption ?? null
    })
    const outputRoute =
      origin === 'human' ? createOutputRoute(session, message.projectId, fromUserId, true) : null
    const buildPrompt = () =>
      buildRemoteImAicliPrompt(
        { fromUserId, text: taskText, replyId: outputRoute?.replyId },
        { includeReplyProtocol: origin === 'human' && !usesSourceLevelRouting(session) }
      )
    const displayText = buildRemoteImAicliDisplayText({
      fromUserId,
      text: taskText
    })
    deps.store.updateStatus(incoming.id, { sessionId: session.sessionId, error: null })
    const sendResult = outputRoute
      ? await sendUserWithOutputRoute(outputRoute, buildPrompt, displayText)
      : await sendMachineInput(session.sessionId, buildPrompt(), displayText)
    if (!sendResult.ok) {
      const error = sendResult.error ?? 'failed to send file message to AICLI'
      deps.store.updateStatus(incoming.id, { status: 'failed', error })
      await sendIncomingFailureIfHuman(
        deps,
        origin,
        message.projectId,
        fromUserId,
        `发送给 AICLI 失败：${error}`
      )
      return { ok: false, error }
    }
    deps.store.updateStatus(incoming.id, {
      sessionId: session.sessionId,
      status: 'sent-to-aicli',
      sentToAicliAt: deps.now?.() ?? Date.now(),
      error: null
    })
    return {
      ok: true,
      aicliSessionId: session.sessionId,
      ...(outputRoute?.replyId ? { replyId: outputRoute.replyId } : {})
    }
  }

  // SDK 漫游补拉（登录后补充离线期间的历史）：只入库展示、绝不路由——漫游是
  // 历史消息，重放 /控制命令 或转发 AICLI 都会造成误触发。返回真实插入条数，
  // 供调用方决定是否广播 messages-changed。
  async function backfillRoamedText(
    projectId: string,
    messages: RemoteImRoamedTextMessage[]
  ): Promise<{ ok: true; inserted: number }> {
    const config = deps.getConfig(projectId)
    const now = deps.now?.() ?? Date.now()
    let inserted = 0
    for (const roamed of messages) {
      const remoteMessageId = roamed.remoteMessageId?.trim()
      const text = roamed.text?.trim()
      if (!remoteMessageId || !text) continue
      if (isRemoteDesktopSignalText(text)) continue
      if (deps.store.findByRemoteMessageId?.('tencent-im', remoteMessageId)) continue

      if (roamed.flow === 'out') {
        // 本端（master 账号）发出的历史：按已送达出站补录。
        deps.store.create({
          projectId,
          sessionId: null,
          provider: 'tencent-im',
          remoteMessageId,
          fromUserId: roamed.fromUserId,
          toUserId: roamed.toUserId ?? null,
          role: roamed.origin === 'machine' ? 'aicli' : 'remote-user',
          direction: 'outgoing',
          content: roamed.text,
          kind: 'text',
          attachment: null,
          status: 'sent-to-im',
          error: null,
          createdAt: roamed.createdAt ?? now,
          sentToAicliAt: null,
          sentToImAt: roamed.createdAt ?? now
        })
        inserted += 1
        continue
      }

      const fromUserId = roamed.fromUserId.trim()
      if (!fromUserId || !getRemoteImPeerRelation(config, fromUserId)) continue

      const incoming: RemoteImIncomingTextMessage = {
        projectId,
        remoteMessageId,
        fromUserId,
        toUserId: roamed.toUserId ?? null,
        text: roamed.text,
        origin: roamed.origin,
        ...(roamed.createdAt !== undefined ? { createdAt: roamed.createdAt } : {})
      }
      // 与实时链路一致的展示分类（AICLI 输出识别决定气泡角色），但不执行任何路由。
      const aicliOutput = parseRemoteImAicliOutputText(text)
      if (aicliOutput !== null || roamed.origin === 'machine') {
        deps.store.create(
          createIncomingRecord(
            { ...incoming, text: aicliOutput ?? text },
            'received',
            null,
            now,
            'aicli'
          )
        )
      } else {
        deps.store.create(createIncomingRecord(incoming, 'received', null, now))
      }
      inserted += 1
    }
    return { ok: true, inserted }
  }

  return {
    handleIncomingText,
    handleIncomingAudio,
    handleIncomingImage,
    handleIncomingFile,
    backfillRoamedText
  }
}
