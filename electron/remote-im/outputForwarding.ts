import type { CreateRemoteImMessageInput } from './messageStore.js'
import { createOutputChunks } from './outputBuffer.js'
import { extractRemoteImReplyOutput } from './replyProtocol.js'
import type { RemoteImConfig } from './types.js'

export const REMOTE_IM_OPERATION_COMPLETE_TEXT = '操作已完成。'
export const REMOTE_IM_AICLI_OUTPUT_PREFIX = '【AICLI 输出】\n'

export function createRemoteImAicliOutputText(text: string): string {
  return `${REMOTE_IM_AICLI_OUTPUT_PREFIX}${text}`
}

export function parseRemoteImAicliOutputText(text: string): string | null {
  if (!text.startsWith(REMOTE_IM_AICLI_OUTPUT_PREFIX)) return null
  return text.slice(REMOTE_IM_AICLI_OUTPUT_PREFIX.length).trim()
}

export interface RemoteImOutputCompletionInfo {
  exitCode?: number | null
  signal?: number | string | null
}

export type RemoteImOutputFlushTimer = ReturnType<typeof setTimeout>

export interface RemoteImOutputSessionState {
  projectId: string
  toUserId: string
  config: RemoteImConfig
  buffer: string
  timer: RemoteImOutputFlushTimer | null
}

export interface RemoteImOutputForwardingDeps {
  createMessage(input: CreateRemoteImMessageInput): void
  sendText(projectId: string, toUserId: string, text: string): void
  messagesChanged(projectId: string | null): void
  now?: () => number
  clearTimer?: (timer: RemoteImOutputFlushTimer) => void
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

export function isRemoteImOperationFinishedText(text: string): boolean {
  return (
    text === REMOTE_IM_OPERATION_COMPLETE_TEXT ||
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
  const reply = extractRemoteImReplyOutput(state.buffer)
  const buffer = reply.content
  state.buffer = reply.nextBuffer
  clearOutputTimer(state, deps)
  if (!buffer.trim()) return 0

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
  return chunks.length
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
