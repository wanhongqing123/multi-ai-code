import type { CreateRemoteImMessageInput } from './messageStore.js'
import { createOutputChunks } from './outputBuffer.js'
import {
  sanitizeRemoteImAicliOutput,
  type RemoteImAicliOutputSourceKind
} from './outputSanitizer.js'
import { extractRemoteImReplyOutput } from './replyProtocol.js'
import type { RemoteImConfig } from './types.js'

export const REMOTE_IM_OPERATION_COMPLETE_TEXT = '操作已完成。'
export const REMOTE_IM_OPERATION_FAILED_PREFIX = 'AICLI 执行失败：'
// Invisible protocol marker: forwarded AICLI output must still be distinguishable
// from user input, but the marker should not appear in IM clients.
export const REMOTE_IM_AICLI_OUTPUT_PREFIX = '\u2063\u200B\u200C\u200D\u2063'
const LEGACY_REMOTE_IM_AICLI_OUTPUT_PREFIXES = [
  '【AICLI 输出】\n',
  '【AICLI 输出】',
  '[AICLI 输出]\n',
  '[AICLI 输出]',
  '【AICLI输出】\n',
  '【AICLI输出】',
  '[AICLI输出]\n',
  '[AICLI输出]'
]

export function createRemoteImAicliOutputText(text: string): string {
  return `${REMOTE_IM_AICLI_OUTPUT_PREFIX}${text}`
}

export function parseRemoteImAicliOutputText(text: string): string | null {
  if (text.startsWith(REMOTE_IM_AICLI_OUTPUT_PREFIX)) {
    return text.slice(REMOTE_IM_AICLI_OUTPUT_PREFIX.length).trim()
  }
  for (const prefix of LEGACY_REMOTE_IM_AICLI_OUTPUT_PREFIXES) {
    if (text.startsWith(prefix)) return text.slice(prefix.length).trim()
  }
  return null
}

export interface RemoteImOutputCompletionInfo {
  exitCode?: number | null
  signal?: number | string | null
}

export type RemoteImOutputFlushTimer = ReturnType<typeof setTimeout>

export interface RemoteImTranscriptSource {
  kind: 'claude'
  cwd: string
  sinceMs: number
  replyId?: string
}

export interface RemoteImOutputSessionState {
  projectId: string
  toUserId: string
  config: RemoteImConfig
  replyId?: string
  sourceKind?: RemoteImAicliOutputSourceKind
  buffer: string
  timer: RemoteImOutputFlushTimer | null
  transcript?: RemoteImTranscriptSource
  forwardedTranscriptReply?: string
  forwardedReplyId?: string
  structuredOutput?: boolean
  taskId?: string
  sourceStarted?: boolean
  lastActivityAt?: number
  forwardedStructuredAssistantEventIds?: Set<string>
  forwardedStructuredAssistantTexts?: string[]
  forwardedStructuredTerminalMessageIds?: Set<string>
}

export interface RemoteImOutputForwardingDeps {
  createMessage(input: CreateRemoteImMessageInput): void
  sendText(projectId: string, toUserId: string, text: string): void
  messagesChanged(projectId: string | null): void
  readTranscriptReply?: (source: RemoteImTranscriptSource) => string | null
  now?: () => number
  clearTimer?: (timer: RemoteImOutputFlushTimer) => void
}

export type RemoteImStructuredFinalContentSource =
  'expected-marker' | 'fallback-marker' | 'pending-marker' | 'markerless' | 'empty'

export interface RemoteImStructuredFinalContent {
  content: string
  source: RemoteImStructuredFinalContentSource
  markerReplyIdMismatch: boolean
}

export function createRemoteImOperationFinishedText(
  info: RemoteImOutputCompletionInfo = {}
): string {
  if (info.signal !== undefined && info.signal !== null) {
    return `操作已结束（信号：${String(info.signal)}）。`
  }
  if (typeof info.exitCode === 'number' && info.exitCode !== 0) {
    return `操作已结束（退出码：${info.exitCode}）。`
  }
  return REMOTE_IM_OPERATION_COMPLETE_TEXT
}

export function createRemoteImOperationFailedText(reason: string): string {
  const message = reason.trim() || '未知错误'
  return `${REMOTE_IM_OPERATION_FAILED_PREFIX}${message}`
}

export function isRemoteImOperationFinishedText(text: string): boolean {
  return (
    text === REMOTE_IM_OPERATION_COMPLETE_TEXT ||
    text.startsWith(REMOTE_IM_OPERATION_FAILED_PREFIX) ||
    text.startsWith('操作已结束（退出码：') ||
    text.startsWith('操作已结束（信号：')
  )
}

function clearOutputTimer(
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps
): void {
  if (!state.timer) return
  ;(deps.clearTimer ?? clearTimeout)(state.timer)
  state.timer = null
}

function createOutgoingMessage(input: {
  sessionId: string
  state: RemoteImOutputSessionState
  content: string
  role: CreateRemoteImMessageInput['role']
  now: number
}): CreateRemoteImMessageInput {
  return {
    projectId: input.state.projectId,
    sessionId: input.sessionId,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: null,
    toUserId: input.state.toUserId,
    role: input.role,
    direction: 'outgoing',
    content: input.content,
    status: 'sent-to-im',
    createdAt: input.now,
    sentToImAt: input.now
  }
}

export function flushRemoteImOutputSession(
  sessionId: string,
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps
): number {
  const transcriptReply =
    state.transcript?.kind === 'claude' && deps.readTranscriptReply
      ? deps.readTranscriptReply(state.transcript)
      : null
  const reply =
    transcriptReply === null
      ? extractRemoteImReplyOutput(state.buffer, { replyId: state.replyId })
      : null
  if (reply && state.replyId && state.forwardedReplyId === state.replyId) {
    state.buffer = reply.nextBuffer
    clearOutputTimer(state, deps)
    return 0
  }
  const buffer = sanitizeRemoteImAicliOutput(transcriptReply ?? reply?.content ?? '', {
    sourceKind: state.sourceKind
  })
  state.buffer = transcriptReply === null ? (reply?.nextBuffer ?? '') : ''
  clearOutputTimer(state, deps)
  if (!buffer.trim()) return 0
  if (transcriptReply !== null) {
    if (state.forwardedTranscriptReply === transcriptReply) return 0
    state.forwardedTranscriptReply = transcriptReply
  }

  const chunks = createOutputChunks(buffer, {
    maxChunkChars: state.config.outputMaxChunkChars
  })
  const now = deps.now?.() ?? Date.now()

  for (const chunk of chunks) {
    deps.createMessage(
      createOutgoingMessage({
        sessionId,
        state,
        content: chunk,
        role: 'aicli',
        now
      })
    )
    deps.sendText(state.projectId, state.toUserId, createRemoteImAicliOutputText(chunk))
  }

  if (chunks.length > 0) deps.messagesChanged(state.projectId)
  if (chunks.length > 0 && reply && state.replyId) state.forwardedReplyId = state.replyId
  return chunks.length
}

/**
 * Structured terminal metadata already identifies the owning task. The text marker is
 * therefore a content envelope, not a second routing authority. This matters when a
 * second remote message steers the same running turn: the model may still close its
 * answer with the earlier marker while the terminal event correctly targets the newer
 * task id.
 */
export function resolveRemoteImStructuredFinalContent(
  text: string,
  expectedReplyId?: string
): RemoteImStructuredFinalContent {
  const expected = extractRemoteImReplyOutput(text, {
    replyId: expectedReplyId
  })
  if (expected.content) {
    return {
      content: expected.content,
      source: 'expected-marker',
      markerReplyIdMismatch: false
    }
  }

  const hasReplyMarker = /<\/?remote-im-reply(?:\s|>)/.test(text)
  if (hasReplyMarker) {
    const markerReplyIds = [
      ...text.matchAll(/<remote-im-reply\s+id="([A-Za-z0-9_-]{1,80})">/g)
    ].map((match) => match[1])
    const fallback =
      markerReplyIds
        .reverse()
        .map((replyId) => extractRemoteImReplyOutput(text, { replyId }))
        .find((candidate) => candidate.content || candidate.pending) ??
      extractRemoteImReplyOutput(text)
    if (fallback.content) {
      return {
        content: fallback.content,
        source: 'fallback-marker',
        markerReplyIdMismatch: Boolean(expectedReplyId)
      }
    }

    const pending = expected.pending ? expected : fallback
    const pendingBody = pending.pending
      ? pending.nextBuffer.split('\n').slice(1).join('\n').trim()
      : ''
    if (pendingBody) {
      return {
        content: pendingBody,
        source: 'pending-marker',
        markerReplyIdMismatch: false
      }
    }

    return {
      content: '',
      source: 'empty',
      markerReplyIdMismatch: false
    }
  }

  return {
    content: text,
    source: text.trim() ? 'markerless' : 'empty',
    markerReplyIdMismatch: false
  }
}

/**
 * Forward a source-confirmed final assistant response. Normal streaming output still
 * requires reply markers; only this terminal event may fall back to markerless text.
 */
export function forwardRemoteImStructuredFinalOutput(
  sessionId: string,
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps,
  text: string,
  messageId?: string
): number {
  clearOutputTimer(state, deps)
  state.buffer = ''
  if (messageId && state.forwardedStructuredTerminalMessageIds?.has(messageId)) return 0

  const resolved = resolveRemoteImStructuredFinalContent(text, state.replyId)
  const buffer = sanitizeRemoteImAicliOutput(resolved.content, {
    sourceKind: state.sourceKind
  })
  state.buffer = ''
  if (!buffer.trim()) return 0
  if (wasStructuredAssistantTextForwarded(state, buffer)) {
    if (messageId) {
      const forwardedIds =
        state.forwardedStructuredTerminalMessageIds ??
        (state.forwardedStructuredTerminalMessageIds = new Set())
      rememberStructuredEventId(forwardedIds, messageId)
    }
    return 0
  }

  const chunks = createOutputChunks(buffer, {
    maxChunkChars: state.config.outputMaxChunkChars
  })
  const now = deps.now?.() ?? Date.now()
  for (const chunk of chunks) {
    deps.createMessage(
      createOutgoingMessage({
        sessionId,
        state,
        content: chunk,
        role: 'aicli',
        now
      })
    )
    deps.sendText(state.projectId, state.toUserId, createRemoteImAicliOutputText(chunk))
  }

  if (chunks.length > 0) {
    deps.messagesChanged(state.projectId)
    if (messageId) {
      const forwardedIds =
        state.forwardedStructuredTerminalMessageIds ??
        (state.forwardedStructuredTerminalMessageIds = new Set())
      rememberStructuredEventId(forwardedIds, messageId)
    }
  }
  return chunks.length
}

/**
 * Forward one source-authored assistant message immediately. Reliable source
 * retransmissions are deduplicated by their event identity, never by replyId or text.
 */
export function forwardRemoteImStructuredAssistantOutput(
  sessionId: string,
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps,
  text: string,
  messageId?: string,
  partId?: string
): number {
  const eventId = messageId ? `${messageId}\u0000${partId ?? ''}` : undefined
  if (eventId && state.forwardedStructuredAssistantEventIds?.has(eventId)) return 0

  const raw = text
  state.buffer = ''
  const reply = extractRemoteImReplyOutput(raw, { replyId: state.replyId })
  const hasReplyMarker = /<\/?remote-im-reply(?:\s|>)/.test(raw)
  const pendingBody = reply.pending ? reply.nextBuffer.split('\n').slice(1).join('\n') : ''
  const candidate = sanitizeRemoteImAicliOutput(
    reply.content || pendingBody || (hasReplyMarker ? '' : raw),
    { sourceKind: state.sourceKind }
  )
  if (!candidate.trim()) return 0

  const chunks = createOutputChunks(candidate, {
    maxChunkChars: state.config.outputMaxChunkChars
  })
  const now = deps.now?.() ?? Date.now()
  for (const chunk of chunks) {
    deps.createMessage(
      createOutgoingMessage({
        sessionId,
        state,
        content: chunk,
        role: 'aicli',
        now
      })
    )
    deps.sendText(state.projectId, state.toUserId, createRemoteImAicliOutputText(chunk))
  }

  if (chunks.length > 0) {
    if (eventId) {
      const forwardedIds =
        state.forwardedStructuredAssistantEventIds ??
        (state.forwardedStructuredAssistantEventIds = new Set())
      rememberStructuredEventId(forwardedIds, eventId)
    }
    const forwardedTexts =
      state.forwardedStructuredAssistantTexts ??
      (state.forwardedStructuredAssistantTexts = [])
    forwardedTexts.push(normalizeStructuredAssistantText(candidate))
    if (forwardedTexts.length > 64) forwardedTexts.splice(0, forwardedTexts.length - 64)
    deps.messagesChanged(state.projectId)
  }
  return chunks.length
}

function rememberStructuredEventId(ids: Set<string> | undefined, id: string | undefined): void {
  if (!ids || !id) return
  ids.add(id)
  if (ids.size <= 128) return
  const oldestId = ids.values().next().value
  if (oldestId) ids.delete(oldestId)
}

function normalizeStructuredAssistantText(text: string): string {
  return text.replace(/\r\n?/g, '\n').trim()
}

function wasStructuredAssistantTextForwarded(
  state: RemoteImOutputSessionState,
  finalText: string
): boolean {
  let remaining = normalizeStructuredAssistantText(finalText)
  const forwarded = state.forwardedStructuredAssistantTexts ?? []
  for (let index = forwarded.length - 1; index >= 0; index -= 1) {
    const current = normalizeStructuredAssistantText(forwarded[index] ?? '')
    if (!current) continue
    if (!remaining.endsWith(current)) return false
    remaining = remaining.slice(0, -current.length).trim()
    if (!remaining) return true
  }
  return false
}

export function completeRemoteImOutputSession(
  sessionId: string,
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps,
  info: RemoteImOutputCompletionInfo = {}
): void {
  flushRemoteImOutputSession(sessionId, state, deps)

  const now = deps.now?.() ?? Date.now()
  const text = createRemoteImOperationFinishedText(info)
  deps.createMessage(
    createOutgoingMessage({
      sessionId,
      state,
      content: text,
      role: 'system',
      now
    })
  )
  deps.sendText(state.projectId, state.toUserId, text)
  deps.messagesChanged(state.projectId)
}

export function failRemoteImOutputSession(
  sessionId: string,
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps,
  reason: string
): void {
  flushRemoteImOutputSession(sessionId, state, deps)

  const now = deps.now?.() ?? Date.now()
  const text = createRemoteImOperationFailedText(reason)
  deps.createMessage(
    createOutgoingMessage({
      sessionId,
      state,
      content: text,
      role: 'system',
      now
    })
  )
  deps.sendText(state.projectId, state.toUserId, text)
  deps.messagesChanged(state.projectId)
}
