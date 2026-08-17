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
// Legacy text envelope retained only for reading history/messages produced by
// older clients. New transports carry the machine origin in cloudCustomData.
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
  pendingReplyIds?: string[]
}

export interface RemoteImTranscriptReply {
  content: string
  /** True only for an exact expected-id assistant frame in Claude's transcript. */
  completed: boolean
  /** The exact marker carried by this assistant frame, when known. */
  replyId?: string
  /** Pending prompts materialized as user turns before this exact assistant frame. */
  completedReplyIds?: string[]
  /** Stable assistant transcript identity used instead of reply-id-wide deduplication. */
  frameId?: string
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
  forwardedReplyIds?: Set<string>
  forwardedTranscriptFrameIds?: Set<string>
  /** Exact Claude reply marker consumed for this route, even when its body is empty. */
  consumedReplyId?: string
  /** Fresh human submission ids still awaiting a causal assistant frame. */
  pendingReplyIds?: string[]
  /** PTY saw a closed frame; poll the trusted assistant transcript before releasing. */
  awaitingTranscriptCompletion?: boolean
  transcriptCompletionPolls?: number
  structuredOutput?: boolean
  taskId?: string
  sourceStarted?: boolean
  lastActivityAt?: number
  forwardedStructuredAssistantEventIds?: Set<string>
  forwardedStructuredAssistantTexts?: string[]
  forwardedStructuredTerminalMessageIds?: Set<string>
  /** False after local takeover or authority revocation: consume lifecycle without IM output. */
  autoReplyToIm?: boolean
  /** Security revocation is terminal for authority and must never be reclaimed by a later steer. */
  authorityRevoked?: boolean
}

/**
 * Stop buffered/non-structured output without emitting another IM message.
 * Keep the route as an admission tombstone until its real reply marker or
 * process exit: deleting it immediately would allow another account/contact to
 * inject into the still-running Claude turn and inherit mixed output.
 */
export function revokeRemoteImOutputSessions(
  sessions: Map<string, RemoteImOutputSessionState>,
  rawUserIds?: Iterable<string>,
  clearTimer: (timer: RemoteImOutputFlushTimer) => void = globalThis.clearTimeout
): string[] {
  const userIds = rawUserIds
    ? new Set([...rawUserIds].map((userId) => userId.trim()).filter(Boolean))
    : null
  const revokedSessionIds: string[] = []
  for (const [sessionId, state] of sessions) {
    if (userIds && !userIds.has(state.toUserId.trim())) continue
    if (state.timer) clearTimer(state.timer)
    state.timer = null
    state.buffer = ''
    state.autoReplyToIm = false
    state.authorityRevoked = true
    revokedSessionIds.push(sessionId)
  }
  return revokedSessionIds
}

export interface RemoteImOutputForwardingDeps {
  createMessage(input: CreateRemoteImMessageInput): unknown
  sendText(projectId: string, toUserId: string, text: string, messageId?: number): void
  messagesChanged(projectId: string | null): void
  readTranscriptReply?: (source: RemoteImTranscriptSource) => RemoteImTranscriptReply | null
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

function shouldAutoReplyToIm(state: RemoteImOutputSessionState): boolean {
  return state.autoReplyToIm !== false
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
    status: 'streaming',
    createdAt: input.now,
    sentToImAt: null
  }
}

function createdMessageId(value: unknown): number | undefined {
  if (!value || typeof value !== 'object' || !('id' in value)) return undefined
  return typeof value.id === 'number' ? value.id : undefined
}

function rememberBoundedId(ids: Set<string>, id: string, limit = 128): void {
  ids.add(id)
  if (ids.size <= limit) return
  const oldest = ids.values().next().value
  if (oldest) ids.delete(oldest)
}

function claudePendingReplyIds(state: RemoteImOutputSessionState): string[] {
  if (state.pendingReplyIds !== undefined) {
    return [...new Set(state.pendingReplyIds.filter(Boolean))]
  }
  return state.replyId ? [state.replyId] : []
}

function extractBufferedRemoteImReply(
  state: RemoteImOutputSessionState
): { replyId?: string; extraction: ReturnType<typeof extractRemoteImReplyOutput> } {
  const replyIds = state.sourceKind === 'claude'
    ? claudePendingReplyIds(state)
    : state.replyId
      ? [state.replyId]
      : []
  if (replyIds.length === 0) {
    return { extraction: extractRemoteImReplyOutput(state.buffer) }
  }
  const candidates = replyIds.map((replyId) => ({
    replyId,
    extraction: extractRemoteImReplyOutput(state.buffer, { replyId })
  }))
  return (
    [...candidates]
      .reverse()
      .find(({ extraction }) => extraction.completed || extraction.content || extraction.pending) ??
    candidates[0]!
  )
}

export function isRemoteImClaudeRouteConsumed(state: RemoteImOutputSessionState): boolean {
  if (state.sourceKind !== 'claude') return false
  if (state.pendingReplyIds) return state.pendingReplyIds.length === 0
  return Boolean(state.replyId && state.consumedReplyId === state.replyId)
}

export function reserveRemoteImClaudeReplyId(
  state: RemoteImOutputSessionState,
  rawReplyId: string | undefined
): boolean {
  const replyId = rawReplyId?.trim()
  if (state.sourceKind !== 'claude' || !replyId) return false
  const pending = state.pendingReplyIds ?? (state.pendingReplyIds = [])
  if (pending.includes(replyId)) return false
  pending.push(replyId)
  if (state.transcript) state.transcript.pendingReplyIds = [...pending]
  return true
}

export function rollbackRemoteImClaudeReplyId(
  state: RemoteImOutputSessionState,
  rawReplyId: string | undefined
): boolean {
  const replyId = rawReplyId?.trim()
  if (state.sourceKind !== 'claude' || !replyId || !state.pendingReplyIds?.includes(replyId)) {
    return false
  }
  state.pendingReplyIds = state.pendingReplyIds.filter((pendingId) => pendingId !== replyId)
  if (state.transcript) state.transcript.pendingReplyIds = [...state.pendingReplyIds]
  return true
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
  const buffered = extractBufferedRemoteImReply(state)
  const reply = buffered.extraction
  const pendingReplyIds = claudePendingReplyIds(state)
  const completedClaudeReplyIds =
    state.sourceKind === 'claude' && transcriptReply?.completed
      ? (transcriptReply.completedReplyIds !== undefined
          ? transcriptReply.completedReplyIds
          : [transcriptReply.replyId ?? state.replyId].filter(
              (replyId): replyId is string => Boolean(replyId)
            )
        ).filter((replyId) => pendingReplyIds.includes(replyId))
      : []
  if (completedClaudeReplyIds.length > 0) {
    const completed = new Set(completedClaudeReplyIds)
    state.pendingReplyIds = pendingReplyIds.filter((replyId) => !completed.has(replyId))
    if (state.transcript) state.transcript.pendingReplyIds = [...state.pendingReplyIds]
    state.consumedReplyId = transcriptReply?.replyId ?? completedClaudeReplyIds.at(-1)
    state.awaitingTranscriptCompletion = state.pendingReplyIds.length > 0
    state.transcriptCompletionPolls = 0
  } else if (
    state.sourceKind === 'claude' &&
    buffered.replyId &&
    reply.completed &&
    !state.forwardedReplyIds?.has(buffered.replyId)
  ) {
    if (!state.awaitingTranscriptCompletion) state.transcriptCompletionPolls = 0
    state.awaitingTranscriptCompletion = true
  }
  const outputReplyId = transcriptReply?.replyId ?? buffered.replyId
  const transcriptFrameAlreadyForwarded = Boolean(
    transcriptReply?.frameId &&
      state.forwardedTranscriptFrameIds?.has(transcriptReply.frameId)
  )
  const replyAlreadyForwarded = Boolean(
    outputReplyId &&
      (state.forwardedReplyIds?.has(outputReplyId) ||
        (state.sourceKind !== 'claude' && state.forwardedReplyId === outputReplyId))
  )
  const buffer = sanitizeRemoteImAicliOutput(transcriptReply?.content ?? reply.content, {
    sourceKind: state.sourceKind
  })
  state.buffer = transcriptReply === null ? (reply?.nextBuffer ?? '') : ''
  clearOutputTimer(state, deps)
  if (transcriptReply?.frameId) {
    const ids =
      state.forwardedTranscriptFrameIds ??
      (state.forwardedTranscriptFrameIds = new Set())
    rememberBoundedId(ids, transcriptReply.frameId)
  }
  if (transcriptFrameAlreadyForwarded || replyAlreadyForwarded) return 0
  if (!buffer.trim()) return 0
  if (transcriptReply !== null && !transcriptReply.frameId) {
    if (state.forwardedTranscriptReply === transcriptReply.content) return 0
    state.forwardedTranscriptReply = transcriptReply.content
  }
  if (!shouldAutoReplyToIm(state)) {
    if (outputReplyId) {
      const ids = state.forwardedReplyIds ?? (state.forwardedReplyIds = new Set())
      rememberBoundedId(ids, outputReplyId)
      if (state.sourceKind !== 'claude') state.forwardedReplyId = outputReplyId
    }
    return 0
  }

  const chunks = createOutputChunks(buffer, {
    maxChunkChars: state.config.outputMaxChunkChars
  })
  const now = deps.now?.() ?? Date.now()

  for (const chunk of chunks) {
    const messageId = createdMessageId(deps.createMessage(
      createOutgoingMessage({
        sessionId,
        state,
        content: chunk,
        role: 'aicli',
        now
      })
    ))
    deps.sendText(state.projectId, state.toUserId, chunk, messageId)
  }

  if (chunks.length > 0) deps.messagesChanged(state.projectId)
  if (chunks.length > 0 && outputReplyId) {
    const ids = state.forwardedReplyIds ?? (state.forwardedReplyIds = new Set())
    rememberBoundedId(ids, outputReplyId)
    if (state.sourceKind !== 'claude') state.forwardedReplyId = outputReplyId
  }
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
  if (!shouldAutoReplyToIm(state)) {
    if (messageId) {
      const forwardedIds =
        state.forwardedStructuredTerminalMessageIds ??
        (state.forwardedStructuredTerminalMessageIds = new Set())
      rememberStructuredEventId(forwardedIds, messageId)
    }
    return 0
  }
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
    const outgoingMessageId = createdMessageId(deps.createMessage(
      createOutgoingMessage({
        sessionId,
        state,
        content: chunk,
        role: 'aicli',
        now
      })
    ))
    deps.sendText(state.projectId, state.toUserId, chunk, outgoingMessageId)
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
  if (!shouldAutoReplyToIm(state)) {
    if (eventId) {
      const forwardedIds =
        state.forwardedStructuredAssistantEventIds ??
        (state.forwardedStructuredAssistantEventIds = new Set())
      rememberStructuredEventId(forwardedIds, eventId)
    }
    return 0
  }

  const chunks = createOutputChunks(candidate, {
    maxChunkChars: state.config.outputMaxChunkChars
  })
  const now = deps.now?.() ?? Date.now()
  for (const chunk of chunks) {
    const outgoingMessageId = createdMessageId(deps.createMessage(
      createOutgoingMessage({
        sessionId,
        state,
        content: chunk,
        role: 'aicli',
        now
      })
    ))
    deps.sendText(state.projectId, state.toUserId, chunk, outgoingMessageId)
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

  // Machine-origin collaboration turns own no automatic IM output path at all.
  // Their model output, success receipt, turn error, signal and non-zero exit
  // all stay local; only an explicit `imcli` call creates the next IM message.
  if (!shouldAutoReplyToIm(state)) return

  const now = deps.now?.() ?? Date.now()
  const text = createRemoteImOperationFinishedText(info)
  const messageId = createdMessageId(deps.createMessage(
    createOutgoingMessage({
      sessionId,
      state,
      content: text,
      role: 'system',
      now
    })
  ))
  deps.sendText(state.projectId, state.toUserId, text, messageId)
  deps.messagesChanged(state.projectId)
}

export function failRemoteImOutputSession(
  sessionId: string,
  state: RemoteImOutputSessionState,
  deps: RemoteImOutputForwardingDeps,
  reason: string
): void {
  flushRemoteImOutputSession(sessionId, state, deps)

  if (!shouldAutoReplyToIm(state)) return

  const now = deps.now?.() ?? Date.now()
  const text = createRemoteImOperationFailedText(reason)
  const messageId = createdMessageId(deps.createMessage(
    createOutgoingMessage({
      sessionId,
      state,
      content: text,
      role: 'system',
      now
    })
  ))
  deps.sendText(state.projectId, state.toUserId, text, messageId)
  deps.messagesChanged(state.projectId)
}
