import { BrowserWindow, ipcMain } from 'electron'
import { promises as fs } from 'fs'
import { basename, join } from 'path'
import {
  addSessionDataListener,
  addSessionExitListener,
  addSessionLocalInputListener,
  getActiveSessionForProject,
  getSessionRuntimeInfo,
  requestAicliBtwForSession,
  requestAicliApprovalForSession,
  requestAicliClearForSession,
  requestAicliCompactForSession,
  requestAicliGoalForSession,
  requestAicliInterruptForSession,
  requestAicliModelForSession,
  requestAicliStatusForSession,
  sendUserMessageToSession,
  switchAicliModeForSession
} from '../cc/ptyManager.js'
import { getActiveAccount, projectDir, rootDir, sanitizeAccountId } from '../store/paths.js'
import {
  readProjectMetaFile,
  writeProjectMetaFile,
  type ProjectMeta
} from '../store/projectMeta.js'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  toRemoteImProjectConfig,
  validateRemoteImConfig
} from './config.js'
import {
  addRemoteImAccountContact,
  hasRemoteImAccountConnectionChanged,
  mergeRemoteImAccountIntoConfig,
  normalizeRemoteImAccountConfig,
  preserveRemoteImAccountContacts,
  readRemoteImAccountConfig,
  removeRemoteImAccountContact,
  removedRemoteImAccountContactUserIds,
  syncRemoteImAccountContactsFromSdk,
  writeRemoteImAccountConfig
} from './account.js'
import {
  clearRemoteImPeerMessages,
  createRemoteImMessage,
  failRemoteImMessageIfStreaming,
  findRemoteImMessageByRemoteId,
  listRemoteImMessageById,
  listRemoteImMessages,
  listRemoteImMessagesForSummary,
  listRemoteImPeerMessagesBefore,
  updateRemoteImMessageStatus
} from './messageStore.js'
import {
  completeRemoteImOutputSession,
  failRemoteImOutputSession,
  forwardRemoteImStructuredAssistantOutput,
  forwardRemoteImStructuredFinalOutput,
  flushRemoteImOutputSession,
  isRemoteImClaudeRouteConsumed,
  reserveRemoteImClaudeReplyId,
  rollbackRemoteImClaudeReplyId,
  revokeRemoteImOutputSessions,
  resolveRemoteImStructuredFinalContent,
  type RemoteImOutputCompletionInfo,
  type RemoteImOutputSessionState
} from './outputForwarding.js'
import {
  RemoteImStructuredTaskRegistry
} from './structuredTaskRegistry.js'
import { getRemoteImAicliOutputSourceKind } from './aicliSourceKind.js'
import {
  createPeerOutgoingFileMessageInput,
  createPeerOutgoingImageMessageInput,
  createPeerOutgoingMessageInput,
  createPeerOutgoingVideoMessageInput,
  resolvePeerUserId
} from './peerMessage.js'
import { getRemoteImAccountProfileId, getRemoteImProfileId } from './profile.js'
import {
  createRemoteImRouter,
  type RemoteImAicliOutputRoute,
  type RemoteImSessionInfo
} from './router.js'
import { appendRemoteImRuntimeLog } from './runtimeLog.js'
import { readLatestClaudeRemoteImReply } from './claudeTranscript.js'
import { startRemoteImCliServer } from './imcliServer.js'
import { addAicliStructuredOutputListener } from '../aicli/structuredOutputBridge.js'
import { executeRemoteImControlCommand } from './controlBridge.js'
import { RemoteImApprovalCoordinator } from './approvalCoordinator.js'
import { createGitDiffReport } from './gitDiffReport.js'
import { createRemoteImAccountChangedStatuses, getRemoteImSendConnectionError } from './status.js'
import {
  cacheRemoteImImage,
  cacheRemoteImImageBytes,
  remoteImImageCacheDirectory,
  type CachedRemoteImImage
} from './imageCache.js'
import {
  loadRemoteImLocalImageForSend,
  readRemoteImLocalImageDataUrl,
  type RemoteImLocalImagePayload
} from './localImageFile.js'
import {
  loadRemoteImLocalFileForSend,
  mimeTypeFromRemoteImFilePath,
  type RemoteImLocalFilePayload
} from './localFile.js'
import {
  loadRemoteImLocalVideoForSend,
  type RemoteImLocalVideoPayload
} from './localVideoFile.js'
import { cacheRemoteImFile, fileAttachmentFromIncoming } from './fileCache.js'
import type {
  RemoteImAccountConfig,
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingFileMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingTextMessage,
  RemoteImFileAttachment,
  RemoteImImageAttachment,
  RemoteImMessageOrigin,
  RemoteImRoamedTextMessage,
  RemoteImRuntimeIdentity,
  ReadRemoteImImagePreviewInput,
  RemoteImRuntimeLogEntryInput,
  RemoteImLoginState,
  RemoteImStatus
} from './types.js'

const REMOTE_IM_META_KEY = 'remote_im_config'
const DEFAULT_REMOTE_IM_PROFILE_ID = 'default'
// Renderer delivery can spend up to 15s waiting for the current Tencent IM
// runtime and another 15s waiting for the SDK send promise. Keep the main-side
// acknowledgement window above that combined bound so a valid approval notice
// is not canceled just before a legitimate send result arrives.
const OUTGOING_DELIVERY_ACK_TIMEOUT_MS = 35_000
// 视频要整包上传到 COS 再由服务端生成封面，渲染层给它的发送窗口是 120s
// （deliverRemoteImOutgoingVideo）。这里必须盖过「15s 等运行时 + 120s 发送」，
// 否则一段大录屏会在还在上传时就被判成「发送方窗口未确认」。
const OUTGOING_VIDEO_DELIVERY_ACK_TIMEOUT_MS = 150_000
const MAX_REMOTE_IM_IMAGE_BYTES = 20 * 1024 * 1024
// 附件（文件/视频）体积上限，收发共用。
//
// 这不是腾讯的产品上限：IM 的真实上限由该 SDKAppID 的云控配置
// (upload_size_limit.f / .v) 决定，Web SDK 只在云控缺省时兜底 100MB；
// 云控若比这里低，SDK 会在上传前以 2351/2352 打回，失败是响的，不会静默。
//
// 20GB 基本等于「不设限」，是产品上的明确决定。
// 运行时那一层过得去：Electron 33 内置 Node 20.18.3，实测
// buffer.constants.MAX_LENGTH = 34359738367（32GB），20GB 不会撞 Buffer 上限。
// 真正的约束是内存——当前实现把整个附件 readFile 进内存、再结构化克隆过 IPC
// 交给渲染进程，所以实际能走通的体积取决于机器可用内存，远小于这个数。
// 这条链路后续会改成流式，届时这个常量才名副其实。
const MAX_REMOTE_IM_ATTACHMENT_BYTES = 20 * 1024 * 1024 * 1024
const MAX_REMOTE_IM_FILE_BYTES = MAX_REMOTE_IM_ATTACHMENT_BYTES
const MAX_REMOTE_IM_VIDEO_BYTES = MAX_REMOTE_IM_ATTACHMENT_BYTES
// md/html 预览要把全文读进内存渲染，保持独立的小上限。
const MAX_REMOTE_IM_DOC_PREVIEW_BYTES = 5 * 1024 * 1024
const CLAUDE_TRANSCRIPT_COMPLETION_POLL_MS = 250
const MAX_CLAUDE_TRANSCRIPT_COMPLETION_POLLS = 6

const statuses = new Map<string, RemoteImStatus>()
type RemoteImAccountBoundOutputSessionState = RemoteImOutputSessionState & {
  securityGeneration: number
}
const outputSessions = new Map<string, RemoteImAccountBoundOutputSessionState>()
type RemoteImStructuredTaskState = RemoteImAccountBoundOutputSessionState & {
  taskId: string
}
const structuredOutputTasks = new RemoteImStructuredTaskRegistry<RemoteImStructuredTaskState>()
const approvalDeliveryWaiters = new Map<
  number,
  (result: { ok: boolean; error?: string }) => void
>()
interface OutgoingDeliveryAckTimer {
  timer: ReturnType<typeof setTimeout>
  projectId: string
  securityGeneration: number
  runtimeIdentity: RemoteImRuntimeIdentity
}
const outgoingDeliveryAckTimers = new Map<number, OutgoingDeliveryAckTimer>()
interface PendingRemoteImOutputDelivery {
  projectId: string
  messageId: number
  securityGeneration: number
}
const pendingRemoteImOutputDeliveries = new Map<
  number,
  PendingRemoteImOutputDelivery
>()
let remoteImApprovalCoordinator: RemoteImApprovalCoordinator | null = null
let activeRemoteImAccountProfileId: string | null = getRemoteImProfileId()
let remoteImAccountSecurityGeneration = 0
let remoteImAccountTransitioning = false
let remoteImApprovalAuthorityMutationCount = 0
let remoteImAccountBindQueue: Promise<void> = Promise.resolve()
// Assistant output can be split into several SDK messages. Keep those sends in
// source order even when a contact-authority mutation temporarily pauses them
// for a fresh allow-list check.
let remoteImOutputDeliveryQueue: Promise<void> = Promise.resolve()
let remoteImAccountBoundOperationCount = 0
const remoteImAccountBoundOperationDrainWaiters = new Set<() => void>()
const remoteImRuntimeIdentities = new Map<
  string,
  RemoteImRuntimeIdentity & { securityGeneration: number }
>()
// An AICLI process that participated in an old Remote IM security generation
// must not silently reuse the bridge after account/SDK credentials change.
// Routes bind after source admission succeeds; local turns bind on first imcli
// use. A fresh route may rebind the same idle session after the old terminal.
const remoteImCliSessionSecurityGenerations = new Map<string, number>()
const revokedRemoteImCliSessions = new Set<string>()

const REMOTE_IM_ACCOUNT_CHANGING_ERROR = 'Remote IM account is changing'

function isRemoteImAccountSecurityGenerationCurrent(generation: number): boolean {
  return (
    !remoteImAccountTransitioning &&
    remoteImApprovalAuthorityMutationCount === 0 &&
    generation === remoteImAccountSecurityGeneration
  )
}

async function withRemoteImAccountBoundOperation<T>(
  operation: (securityGeneration: number) => Promise<T>
): Promise<T | { ok: false; error: string }> {
  if (remoteImAccountTransitioning || remoteImApprovalAuthorityMutationCount > 0) {
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  const securityGeneration = remoteImAccountSecurityGeneration
  remoteImAccountBoundOperationCount += 1
  try {
    // bindRemoteImAccountConfig sets transitioning before it can change the
    // account-scoped data layer, so this operation either stays wholly on the
    // old account or is rejected before it starts.
    if (!isRemoteImAccountSecurityGenerationCurrent(securityGeneration)) {
      return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
    }
    return await operation(securityGeneration)
  } finally {
    remoteImAccountBoundOperationCount -= 1
    if (remoteImAccountBoundOperationCount === 0) {
      for (const resolve of remoteImAccountBoundOperationDrainWaiters) resolve()
      remoteImAccountBoundOperationDrainWaiters.clear()
    }
  }
}

/**
 * Real-time SDK callbacks must survive a same-account contact mutation. Wait
 * for that mutation, then re-read the current allow-list inside a fresh bound
 * operation. Account switches still reject immediately because the callback's
 * runtime identity belongs to the old connection.
 */
async function withRemoteImIncomingOperation<T>(
  operation: (securityGeneration: number) => Promise<T>
): Promise<T | { ok: false; error: string }> {
  for (;;) {
    if (remoteImAccountTransitioning) {
      return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
    }
    if (remoteImApprovalAuthorityMutationCount > 0) {
      const pendingMutations = remoteImAccountBindQueue
      await pendingMutations
      continue
    }
    const result = await withRemoteImAccountBoundOperation(operation)
    if (
      result &&
      typeof result === 'object' &&
      'ok' in result &&
      result.ok === false &&
      'error' in result &&
      result.error === REMOTE_IM_ACCOUNT_CHANGING_ERROR &&
      !remoteImAccountTransitioning
    ) {
      continue
    }
    return result
  }
}

function waitForRemoteImAccountBoundOperationsToDrain(): Promise<void> {
  if (remoteImAccountBoundOperationCount === 0) return Promise.resolve()
  return new Promise((resolve) => {
    remoteImAccountBoundOperationDrainWaiters.add(resolve)
  })
}

function enqueueRemoteImAccountMutation<T>(operation: () => Promise<T>): Promise<T> {
  const result = remoteImAccountBindQueue.then(operation)
  remoteImAccountBindQueue = result.then(
    () => undefined,
    () => undefined
  )
  return result
}

/**
 * Contact changes alter who is allowed to steer AICLI and approve dangerous
 * commands, but they do not require reconnecting the Tencent runtime. Reserve
 * the account-mutation queue synchronously so no new bound handler can start,
 * then wait for every handler that already captured the old allow-list before
 * changing authority. This makes the revoke/write operation linearizable.
 */
function enqueueRemoteImApprovalAuthorityMutation<T>(
  operation: () => Promise<T>
): Promise<T> {
  remoteImApprovalAuthorityMutationCount += 1

  return enqueueRemoteImAccountMutation(async () => {
    try {
      await waitForRemoteImAccountBoundOperationsToDrain()
      return await operation()
    } finally {
      remoteImApprovalAuthorityMutationCount -= 1
    }
  })
}

function structuredOutputTextPreview(text: string): string {
  return text.replace(/\s+/g, ' ').trim().slice(0, 180)
}

function writeStructuredOutputRuntimeLog(
  event: string,
  input: {
    sessionId: string
    state?: RemoteImOutputSessionState
    detail?: Record<string, unknown>
  }
): void {
  const { state } = input
  void appendRemoteImRuntimeLog(rootDir(), {
    projectId: state?.projectId ?? null,
    sdkAppId: state?.config.sdkAppId ?? null,
    desktopUserId: state?.config.desktopUserId ?? null,
    peerUserId: state?.toUserId ?? null,
    event,
    detail: {
      sessionId: input.sessionId,
      taskId: state?.taskId ?? null,
      replyId: state?.replyId ?? null,
      sourceKind: state?.sourceKind ?? null,
      ...input.detail
    }
  }).catch((err) => {
    console.error(
      '[remote-im] failed to append structured output log:',
      err instanceof Error ? err.message : String(err)
    )
  })
}

function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload)
  }
}

function broadcastStatus(status: RemoteImStatus): void {
  statuses.set(status.projectId ?? '', status)
  broadcast('remote-im:status', status)
}

function resetRemoteImStatusesAfterAccountChange(): void {
  for (const status of createRemoteImAccountChangedStatuses(statuses.values())) {
    broadcastStatus(status)
  }
}

function revokeRemoteImOutputRoutes(
  rawUserIds?: Iterable<string>
): string[] {
  const userIds = rawUserIds
    ? new Set([...rawUserIds].map((userId) => userId.trim()).filter(Boolean))
    : null
  const affectedSessionIds = [...outputSessions.entries()]
    .filter(([, state]) => !userIds || userIds.has(state.toUserId.trim()))
    .map(([sessionId]) => sessionId)

  // A complete Claude frame may already be buffered while its debounce timer
  // is pending. Consume it silently before revocation clears the buffer;
  // otherwise the retained admission tombstone can never observe its terminal
  // marker and may block the next authorized task until process exit.
  for (const sessionId of affectedSessionIds) {
    const state = outputSessions.get(sessionId)
    if (!state) continue
    state.autoReplyToIm = false
    flushRemoteImOutputSession(
      sessionId,
      state,
      outputForwardingDeps(state.securityGeneration)
    )
    if (
      outputSessions.get(sessionId) === state &&
      isRemoteImClaudeRouteConsumed(state)
    ) {
      outputSessions.delete(sessionId)
      writeStructuredOutputRuntimeLog('aicli:claude-reply-frame-consumed', {
        sessionId,
        state,
        detail: {
          replyId: state.replyId,
          forwardedChunks: 0,
          autoReplyToIm: false,
          reason: 'authority-revoked'
        }
      })
    }
  }

  revokeRemoteImOutputSessions(outputSessions, userIds ?? undefined)
  for (const sessionId of affectedSessionIds) {
    const state = outputSessions.get(sessionId)
    if (state?.awaitingTranscriptCompletion) {
      scheduleClaudeTranscriptCompletionPoll(sessionId, state)
    }
  }
  return affectedSessionIds
}

async function invalidateRemoteImSecurityStateForAccountChange(): Promise<void> {
  // Invalidate capabilities before the new credentials/profile become active.
  // The generation check closes races with an in-flight /approve command, while
  // local-takeover tombstones prevent another account from steering the same
  // still-running Codex turn.
  cancelOutgoingDeliveryAckTimeoutsForAccountChange()
  cancelPendingRemoteImOutputDeliveriesForAccountChange()
  remoteImAccountSecurityGeneration += 1
  remoteImRuntimeIdentities.clear()
  revokeRemoteImOutputRoutes()
  for (const sessionId of structuredOutputTasks.sessionIds()) {
    for (const state of structuredOutputTasks.list(sessionId)) {
      state.authorityRevoked = true
    }
    structuredOutputTasks.markLocalTakeover(sessionId)
  }
  await remoteImApprovalCoordinator?.cancelAll()
  for (const [messageId, resolve] of approvalDeliveryWaiters) {
    approvalDeliveryWaiters.delete(messageId)
    resolve({ ok: false, error: 'Remote IM account changed during approval delivery' })
  }
}

function sameRemoteImRuntimeIdentity(
  expected: RemoteImRuntimeIdentity,
  actual: RemoteImRuntimeIdentity
): boolean {
  return (
    expected.connectionId === actual.connectionId &&
    expected.desktopUserId.trim() === actual.desktopUserId.trim() &&
    expected.sdkAppId === actual.sdkAppId
  )
}

function isRegisteredRemoteImRuntime(
  projectId: string,
  identity: RemoteImRuntimeIdentity | null | undefined
): boolean {
  if (!identity) return false
  const current = remoteImRuntimeIdentities.get(projectId)
  return Boolean(
    current &&
      current.securityGeneration === remoteImAccountSecurityGeneration &&
      sameRemoteImRuntimeIdentity(current, identity)
  )
}

function isCurrentRemoteImRuntime(
  projectId: string,
  identity: RemoteImRuntimeIdentity | null | undefined
): boolean {
  return (
    !remoteImAccountTransitioning &&
    remoteImApprovalAuthorityMutationCount === 0 &&
    isRegisteredRemoteImRuntime(projectId, identity)
  )
}

function getRegisteredRemoteImRuntimeIdentity(
  projectId: string
): RemoteImRuntimeIdentity | null {
  const current = remoteImRuntimeIdentities.get(projectId)
  if (
    remoteImAccountTransitioning ||
    remoteImApprovalAuthorityMutationCount > 0 ||
    !current ||
    current.securityGeneration !== remoteImAccountSecurityGeneration
  ) {
    return null
  }
  return {
    connectionId: current.connectionId,
    desktopUserId: current.desktopUserId,
    sdkAppId: current.sdkAppId
  }
}

function broadcastMessagesChanged(projectId: string | null): void {
  broadcast('remote-im:messages-changed', { projectId })
}

function broadcastOutgoingText(
  projectId: string,
  toUserId: string,
  text: string,
  messageId?: number,
  origin: RemoteImMessageOrigin = 'machine'
): boolean {
  const runtimeIdentity = getRegisteredRemoteImRuntimeIdentity(projectId)
  if (!runtimeIdentity) return false
  broadcast('remote-im:outgoing-text', {
    projectId,
    toUserId,
    text,
    origin,
    runtimeIdentity,
    messageId
  })
  return true
}

function broadcastOutgoingImage(
  projectId: string,
  toUserId: string,
  fileToken: string,
  messageId?: number,
  origin: RemoteImMessageOrigin = 'machine'
): boolean {
  const runtimeIdentity = getRegisteredRemoteImRuntimeIdentity(projectId)
  if (!runtimeIdentity) return false
  broadcast('remote-im:outgoing-image', {
    projectId,
    toUserId,
    origin,
    runtimeIdentity,
    fileToken,
    messageId
  })
  return true
}

function broadcastOutgoingImagePayload(
  projectId: string,
  toUserId: string,
  image: RemoteImLocalImagePayload,
  messageId?: number,
  origin: RemoteImMessageOrigin = 'machine'
): boolean {
  const runtimeIdentity = getRegisteredRemoteImRuntimeIdentity(projectId)
  if (!runtimeIdentity) return false
  broadcast('remote-im:outgoing-image', {
    projectId,
    toUserId,
    origin,
    runtimeIdentity,
    messageId,
    fileToken: null,
    fileName: image.fileName,
    mimeType: image.mimeType,
    fileBytes: image.fileBytes
  })
  return true
}

function broadcastOutgoingFilePayload(
  projectId: string,
  toUserId: string,
  file: RemoteImLocalFilePayload,
  messageId?: number,
  origin: RemoteImMessageOrigin = 'machine'
): boolean {
  const runtimeIdentity = getRegisteredRemoteImRuntimeIdentity(projectId)
  if (!runtimeIdentity) return false
  broadcast('remote-im:outgoing-file', {
    projectId,
    toUserId,
    origin,
    runtimeIdentity,
    messageId,
    fileName: file.fileName,
    mimeType: file.mimeType,
    fileBytes: file.fileBytes
  })
  return true
}

function broadcastOutgoingVideoPayload(
  projectId: string,
  toUserId: string,
  video: RemoteImLocalVideoPayload,
  messageId?: number,
  origin: RemoteImMessageOrigin = 'machine'
): boolean {
  const runtimeIdentity = getRegisteredRemoteImRuntimeIdentity(projectId)
  if (!runtimeIdentity) return false
  broadcast('remote-im:outgoing-video', {
    projectId,
    toUserId,
    origin,
    runtimeIdentity,
    messageId,
    fileName: video.fileName,
    mimeType: video.mimeType,
    fileBytes: video.fileBytes
  })
  return true
}

/**
 * 通知渲染层重新拉取项目配置。
 *
 * messages-changed 只会让界面重拉消息列表，config 一动不动——而「好友」页读的
 * 正是 config。imcli 加了好友却要重启才看得见，就是缺这一条。
 */
function broadcastConfigChanged(projectId: string): void {
  broadcast('remote-im:config-changed', { projectId })
}

function scheduleOutgoingDeliveryAckTimeout(
  projectId: string,
  messageId: number,
  timeoutMs: number = OUTGOING_DELIVERY_ACK_TIMEOUT_MS
): void {
  const runtimeIdentity = getRegisteredRemoteImRuntimeIdentity(projectId)
  if (!runtimeIdentity) return
  const existing = outgoingDeliveryAckTimers.get(messageId)
  if (existing) clearTimeout(existing.timer)
  const entry: OutgoingDeliveryAckTimer = {
    projectId,
    securityGeneration: remoteImAccountSecurityGeneration,
    runtimeIdentity,
    timer: setTimeout(() => {
      if (outgoingDeliveryAckTimers.get(messageId) !== entry) return
      outgoingDeliveryAckTimers.delete(messageId)
      if (entry.securityGeneration !== remoteImAccountSecurityGeneration) return
      const error = isRegisteredRemoteImRuntime(entry.projectId, entry.runtimeIdentity)
        ? 'Remote IM sender window did not confirm delivery'
        : 'Remote IM runtime changed before delivery was confirmed'
      const updated = failRemoteImMessageIfStreaming(messageId, error)
      if (updated?.status === 'failed') {
        broadcastMessagesChanged(projectId)
      }
      settleApprovalDelivery(messageId, { ok: false, error })
    }, timeoutMs)
  }
  entry.timer.unref?.()
  outgoingDeliveryAckTimers.set(messageId, entry)
}

function clearOutgoingDeliveryAckTimeout(
  projectId: string,
  messageId: number,
  runtimeIdentity: RemoteImRuntimeIdentity
): void {
  const entry = outgoingDeliveryAckTimers.get(messageId)
  if (
    !entry ||
    entry.projectId !== projectId ||
    !sameRemoteImRuntimeIdentity(entry.runtimeIdentity, runtimeIdentity)
  ) {
    return
  }
  clearTimeout(entry.timer)
  outgoingDeliveryAckTimers.delete(messageId)
}

function cancelOutgoingDeliveryAckTimeoutsForAccountChange(): void {
  for (const [messageId, entry] of outgoingDeliveryAckTimers) {
    clearTimeout(entry.timer)
    outgoingDeliveryAckTimers.delete(messageId)
    const error = 'Remote IM account changed during message delivery'
    const updated = failRemoteImMessageIfStreaming(messageId, error)
    if (updated?.status === 'failed') broadcastMessagesChanged(entry.projectId)
    settleApprovalDelivery(messageId, { ok: false, error })
  }
}

function cancelPendingRemoteImOutputDeliveriesForAccountChange(): void {
  for (const [messageId, delivery] of pendingRemoteImOutputDeliveries) {
    pendingRemoteImOutputDeliveries.delete(messageId)
    const error = 'Remote IM account changed before output delivery was submitted'
    const updated = failRemoteImMessageIfStreaming(messageId, error)
    if (updated?.status === 'failed') {
      broadcastMessagesChanged(delivery.projectId)
    }
  }
}

function settleApprovalDelivery(
  messageId: number,
  result: { ok: boolean; error?: string }
): void {
  const resolve = approvalDeliveryWaiters.get(messageId)
  if (!resolve) return
  approvalDeliveryWaiters.delete(messageId)
  resolve(result)
}

async function readProjectMeta(
  projectId: string
): Promise<{ meta: ProjectMeta; repaired: boolean }> {
  const metaPath = join(projectDir(projectId), 'project.json')
  try {
    const result = await readProjectMetaFile(metaPath)
    if (!result.ok) return { meta: {}, repaired: false }
    return { meta: result.meta, repaired: result.repaired }
  } catch {
    return { meta: {}, repaired: false }
  }
}

async function writeProjectMeta(projectId: string, meta: ProjectMeta): Promise<void> {
  const dir = projectDir(projectId)
  await fs.mkdir(dir, { recursive: true })
  await writeProjectMetaFile(join(dir, 'project.json'), meta)
}

async function getRemoteImConfig(projectId: string): Promise<RemoteImConfig> {
  const { meta } = await readProjectMeta(projectId)
  const projectConfig = normalizeRemoteImConfig(meta[REMOTE_IM_META_KEY])
  const account = await getRemoteImAccountForProject(projectConfig)
  return mergeRemoteImAccountIntoConfig(projectConfig, account)
}

async function getRemoteImAccountForProject(
  projectConfig?: RemoteImConfig
): Promise<RemoteImAccountConfig> {
  const profileId = getCurrentRemoteImAccountProfileId()
  if (profileId) {
    const account = await readRemoteImAccountConfig(remoteImAccountDir(profileId))
    if (
      account.desktopUserId ||
      account.sdkAppId ||
      account.userSigEndpoint ||
      account.userSigSecretKey
    ) {
      return account
    }
  }
  return normalizeRemoteImAccountConfig(projectConfig)
}

async function getRemoteImLoginState(): Promise<RemoteImLoginState> {
  const profileId = getCurrentRemoteImAccountProfileId()
  return {
    profileId,
    account: profileId
      ? await readRemoteImAccountConfig(remoteImAccountDir(profileId))
      : normalizeRemoteImAccountConfig(null)
  }
}

async function getRemoteImAccountByUserId(userId: string): Promise<RemoteImLoginState | null> {
  const profileId = getRemoteImAccountProfileId(userId)
  if (!profileId) return null
  const account = await readRemoteImAccountConfig(remoteImAccountDir(profileId))
  return account.desktopUserId
    ? {
        profileId,
        account
      }
    : null
}

function getCurrentRemoteImAccountProfileId(): string | null {
  return activeRemoteImAccountProfileId ?? getRemoteImProfileId()
}

function remoteImAccountDir(profileId: string): string {
  return join(rootDir(), 'remote-im-profiles', profileId)
}

async function setRemoteImConfig(
  projectId: string,
  rawConfig: unknown
): Promise<
  | { ok: true; value: RemoteImConfig; repaired?: true }
  | {
      ok: false
      error: string
      details?: Array<{ path: string; message: string }>
    }
> {
  const config = toRemoteImProjectConfig(normalizeRemoteImConfig(rawConfig))
  const validation = validateRemoteImConfig(config)
  if (!validation.ok) {
    return {
      ok: false,
      error: 'remote IM config is invalid',
      details: validation.issues.map((issue) => ({
        path: issue.path,
        message: issue.message
      }))
    }
  }

  const { meta, repaired } = await readProjectMeta(projectId)
  meta[REMOTE_IM_META_KEY] = config
  await writeProjectMeta(projectId, meta)
  const mergedConfig = mergeRemoteImAccountIntoConfig(
    config,
    await getRemoteImAccountForProject(config)
  )
  // 这里**不能**广播 disconnected。写项目配置改变不了连接：toRemoteImProjectConfig
  // 会剥掉所有连接相关字段（凭证、账号、provider 都存在账号库里），
  // config.test.ts 的「strips every connection-relevant field」守着这个前提。
  // 状态的真源是渲染层的 remote-im:report-status；账号真的变了由
  // resetRemoteImStatusesAfterAccountChange 单独重置。
  //
  // 曾经这里是无条件广播 disconnected 的，因为当时唯一的调用方是登录流程——
  // 紧接着就重连并上报真实状态，假状态活不过一瞬。等到保存 remoteDesktopMode
  // 也走这条路，而它又故意不触发重连，假状态就再也没人纠正：连接明明好好的，
  // 徽标却一直显示未连接。
  return {
    ok: true,
    value: mergedConfig,
    ...(repaired ? { repaired: true as const } : {})
  }
}

async function getRemoteImStatus(projectId: string): Promise<RemoteImStatus> {
  const existing = statuses.get(projectId)
  if (existing) return existing
  return {
    projectId,
    state: 'disconnected',
    detail: null,
    updatedAt: Date.now()
  }
}

function sendImText(
  projectId: string,
  toUserId: string,
  text: string,
  options: { messageId?: number } = {}
): Promise<{ ok: boolean }> {
  const broadcasted = broadcastOutgoingText(projectId, toUserId, text, options.messageId)
  if (!broadcasted) return Promise.resolve({ ok: false })
  if (options.messageId) {
    scheduleOutgoingDeliveryAckTimeout(projectId, options.messageId)
  }
  return Promise.resolve({ ok: true })
}

async function sendRemoteImApprovalText(
  projectId: string,
  toUserId: string,
  text: string
): Promise<{ ok: boolean; error?: string }> {
  if (remoteImAccountTransitioning || remoteImApprovalAuthorityMutationCount > 0) {
    return { ok: false, error: 'Remote IM account is changing' }
  }
  const securityGeneration = remoteImAccountSecurityGeneration
  const connectionError = getRemoteImSendConnectionError(await getRemoteImStatus(projectId))
  if (connectionError) return { ok: false, error: connectionError }
  if (!getRegisteredRemoteImRuntimeIdentity(projectId)) {
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }
  if (
    remoteImAccountTransitioning ||
    remoteImApprovalAuthorityMutationCount > 0 ||
    securityGeneration !== remoteImAccountSecurityGeneration
  ) {
    return { ok: false, error: 'Remote IM account changed before approval delivery' }
  }

  const now = Date.now()
  const message = createRemoteImMessage({
    projectId,
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: null,
    toUserId,
    role: 'system',
    direction: 'outgoing',
    content: text,
    kind: 'text',
    attachment: null,
    status: 'streaming',
    error: null,
    createdAt: now,
    sentToAicliAt: null,
    sentToImAt: null
  })
  const delivery = new Promise<{ ok: boolean; error?: string }>((resolve) => {
    approvalDeliveryWaiters.set(message.id, resolve)
  })
  const broadcasted = broadcastOutgoingText(
    projectId,
    toUserId,
    text,
    message.id
  )
  if (!broadcasted) {
    approvalDeliveryWaiters.delete(message.id)
    failRemoteImMessageIfStreaming(message.id, 'Remote IM runtime is not connected')
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }
  scheduleOutgoingDeliveryAckTimeout(projectId, message.id)
  broadcastMessagesChanged(projectId)
  // Approval delivery is security-sensitive: do not tell the coordinator that
  // the capability was sent until the renderer reports the Tencent SDK result.
  // A failure/timeout makes the coordinator cancel the Codex request.
  return delivery
}

async function revokeRemoteImApprovalAuthorityForUsers(
  rawUserIds: string[]
): Promise<void> {
  const userIds = new Set(rawUserIds.map((userId) => userId.trim()).filter(Boolean))
  if (userIds.size === 0) return
  const revokedOutputSessionIds = revokeRemoteImOutputRoutes(userIds)
  for (const sessionId of revokedOutputSessionIds) {
    revokedRemoteImCliSessions.add(sessionId)
    const state = outputSessions.get(sessionId)
    if (!state) continue
    writeStructuredOutputRuntimeLog('aicli:route-authority-revoked', {
      sessionId,
      state,
      detail: {
        reason: 'contact-removed',
        admissionLockRetained: true
      }
    })
  }
  for (const sessionId of structuredOutputTasks.sessionIds()) {
    const routes = structuredOutputTasks.list(sessionId)
    if (!routes.some((route) => userIds.has(route.toUserId))) continue
    revokedRemoteImCliSessions.add(sessionId)
    for (const state of structuredOutputTasks.markLocalTakeover(sessionId)) {
      state.authorityRevoked = true
      writeStructuredOutputRuntimeLog('aicli:route-authority-revoked', {
        sessionId,
        state,
        detail: { reason: 'contact-removed' }
      })
    }
  }
  await remoteImApprovalCoordinator?.cancelForRequesters(userIds)
}

function getRemoteImApprovalCoordinator(): RemoteImApprovalCoordinator {
  remoteImApprovalCoordinator ??= new RemoteImApprovalCoordinator({
    sendText: sendRemoteImApprovalText,
    resolveApproval: ({ sessionId, approvalId, threadId, taskId, turnId, decision }) =>
      requestAicliApprovalForSession(sessionId, {
        approvalId,
        threadId,
        taskId,
        turnId,
        decision
    }),
    getSecurityGeneration: () => remoteImAccountSecurityGeneration,
    isSecurityContextCurrent: () =>
      !remoteImAccountTransitioning && remoteImApprovalAuthorityMutationCount === 0
  })
  return remoteImApprovalCoordinator
}

function imageAttachmentFromIncoming(
  message: RemoteImIncomingImageMessage,
  patch: Partial<RemoteImImageAttachment> = {}
): RemoteImImageAttachment {
  return {
    type: 'image',
    localPath: patch.localPath ?? null,
    remoteUrl: patch.remoteUrl ?? (message.imageUrl.trim() || null),
    thumbnailUrl: patch.thumbnailUrl ?? message.thumbnailUrl?.trim() ?? null,
    width: patch.width ?? message.width ?? null,
    height: patch.height ?? message.height ?? null,
    sizeBytes: patch.sizeBytes ?? message.sizeBytes ?? null,
    fileName: patch.fileName ?? message.fileName?.trim() ?? null,
    mimeType: patch.mimeType ?? message.mimeType?.trim() ?? null,
    sdkImageId: patch.sdkImageId ?? message.uuid?.trim() ?? null
  }
}

/**
 * 把一个账号加进好友名单（账号级配置，跨项目生效）。
 *
 * 与界面上「添加联系人」走同一份存储：读账号配置 -> 加名单 -> 落盘 -> 刷新项目配置。
 * 加回一个之前删过的人时，addRemoteImAccountContact 会把 blockedUserIds 里的
 * 墓碑一并清掉，否则 SDK 下次同步又会把他过滤掉。
 */
async function addRemoteImContact(
  projectId: string,
  rawUserId: string
): Promise<{ ok: true; userId: string } | { ok: false; error: string }> {
  const userId = rawUserId.trim()
  if (!userId) return { ok: false, error: '请填写账号 ID' }

  const config = await getRemoteImConfig(projectId)
  if (userId === config.desktopUserId.trim()) {
    return { ok: false, error: '不能把自己加为联系人' }
  }

  const previousProfileId = getCurrentRemoteImAccountProfileId()
  const previousAccount = previousProfileId
    ? await readRemoteImAccountConfig(remoteImAccountDir(previousProfileId))
    : normalizeRemoteImAccountConfig(null)
  const nextAccount = addRemoteImAccountContact(previousAccount, userId)
  const profileId =
    getRemoteImAccountProfileId(nextAccount.desktopUserId) ??
    getRemoteImProfileId() ??
    DEFAULT_REMOTE_IM_PROFILE_ID
  activeRemoteImAccountProfileId = profileId
  await writeRemoteImAccountConfig(remoteImAccountDir(profileId), nextAccount)
  broadcastConfigChanged(projectId)
  broadcastMessagesChanged(projectId)
  return { ok: true, userId }
}

async function deleteRemoteImContact(
  projectId: string,
  rawUserId: string
): Promise<
  { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState } | { ok: false; error: string }
> {
  const userId = rawUserId.trim()
  if (!userId) return { ok: false, error: '请填写账号 ID' }

  // The mutation wrapper has already invalidated every outstanding approval.
  // Tombstone this peer's task before the first filesystem await so no later
  // event from the same Codex turn can mint a fresh capability.
  const previousProfileId = getCurrentRemoteImAccountProfileId()
  const previousAccount = previousProfileId
    ? await readRemoteImAccountConfig(remoteImAccountDir(previousProfileId))
    : normalizeRemoteImAccountConfig(null)
  const nextAccount = removeRemoteImAccountContact(previousAccount, userId)
  const removedContactUserIds = removedRemoteImAccountContactUserIds(
    previousAccount,
    nextAccount
  )
  await revokeRemoteImApprovalAuthorityForUsers(removedContactUserIds)
  const profileId =
    getRemoteImAccountProfileId(nextAccount.desktopUserId) ??
    getRemoteImProfileId() ??
    DEFAULT_REMOTE_IM_PROFILE_ID
  activeRemoteImAccountProfileId = profileId
  const account = await writeRemoteImAccountConfig(remoteImAccountDir(profileId), nextAccount)
  clearRemoteImPeerMessages(projectId, userId)
  const value = await getRemoteImConfig(projectId)
  broadcastMessagesChanged(projectId)
  return {
    ok: true,
    value,
    loginState: {
      profileId,
      account
    }
  }
}

async function syncRemoteImContactsFromSdk(
  projectId: string,
  rawUserIds: string[],
  runtimeIdentity: RemoteImRuntimeIdentity
): Promise<
  { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState } | { ok: false; error: string }
> {
  // This function runs inside the authority-mutation queue, which deliberately
  // blocks all ordinary runtime callbacks. Validate against the registered
  // connection itself rather than asking whether callbacks are globally open.
  if (!isRegisteredRemoteImRuntime(projectId, runtimeIdentity)) {
    return { ok: false, error: '远程 IM 连接已失效，忽略旧连接的通讯录更新' }
  }
  const profileId = getCurrentRemoteImAccountProfileId()
  if (!profileId) return { ok: false, error: '远程 IM 账号未登录' }
  const previousAccount = await readRemoteImAccountConfig(remoteImAccountDir(profileId))
  if (!previousAccount.desktopUserId) return { ok: false, error: '远程 IM 账号未登录' }
  const nextAccount = syncRemoteImAccountContactsFromSdk(previousAccount, rawUserIds)
  await revokeRemoteImApprovalAuthorityForUsers(
    removedRemoteImAccountContactUserIds(previousAccount, nextAccount)
  )
  const account = await writeRemoteImAccountConfig(
    remoteImAccountDir(profileId),
    nextAccount
  )
  const value = await getRemoteImConfig(projectId)
  return {
    ok: true,
    value,
    loginState: {
      profileId,
      account
    }
  }
}

async function sendRemoteImPeerMessage(
  projectId: string,
  text: string,
  toUserId?: string | null,
  origin: RemoteImMessageOrigin = 'machine'
): Promise<{ ok: boolean; error?: string; toUserId?: string }> {
  const config = await getRemoteImConfig(projectId)
  const cleanText = text.trim()
  if (!cleanText) return { ok: false, error: 'empty message' }
  const peerUserId = resolvePeerUserId(config, toUserId)
  if (!peerUserId) {
    return { ok: false, error: '未配置远程 IM 联系人账号' }
  }
  const connectionError = getRemoteImSendConnectionError(await getRemoteImStatus(projectId))
  if (connectionError) {
    return { ok: false, error: connectionError }
  }
  if (!getRegisteredRemoteImRuntimeIdentity(projectId)) {
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }

  const message = createRemoteImMessage(
    createPeerOutgoingMessageInput({
      projectId,
      config,
      toUserId: peerUserId,
      text: cleanText,
      now: Date.now()
    })
  )
  if (!broadcastOutgoingText(projectId, peerUserId, cleanText, message.id, origin)) {
    updateRemoteImMessageStatus(message.id, {
      status: 'failed',
      error: REMOTE_IM_ACCOUNT_CHANGING_ERROR
    })
    broadcastMessagesChanged(projectId)
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  scheduleOutgoingDeliveryAckTimeout(projectId, message.id)
  broadcastMessagesChanged(projectId)
  return { ok: true, toUserId: peerUserId }
}

async function sendRemoteImPeerImage(input: {
  projectId: string
  fileToken: string
  toUserId?: string | null
  localPath?: string | null
  fileName?: string | null
  mimeType?: string | null
  sizeBytes?: number | null
  origin?: RemoteImMessageOrigin
}): Promise<{ ok: boolean; error?: string; toUserId?: string }> {
  const config = await getRemoteImConfig(input.projectId)
  const fileToken = input.fileToken.trim()
  if (!fileToken) return { ok: false, error: '图片文件已失效，请重新选择' }
  const peerUserId = resolvePeerUserId(config, input.toUserId)
  if (!peerUserId) {
    return { ok: false, error: '未配置远程 IM 联系人账号' }
  }
  const connectionError = getRemoteImSendConnectionError(await getRemoteImStatus(input.projectId))
  if (connectionError) {
    return { ok: false, error: connectionError }
  }
  if (!getRegisteredRemoteImRuntimeIdentity(input.projectId)) {
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }

  const localPath = input.localPath?.trim()
  if (!localPath) return { ok: false, error: '图片文件已失效，请重新选择' }
  let payload: RemoteImLocalImagePayload
  let cached: CachedRemoteImImage
  try {
    payload = await loadRemoteImLocalImageForSend(localPath, {
      maxBytes: MAX_REMOTE_IM_IMAGE_BYTES
    })
    cached = await cacheRemoteImImageBytes({
      rootDir: rootDir(),
      projectId: input.projectId,
      fileName: payload.fileName,
      mimeType: payload.mimeType,
      bytes: payload.fileBytes
    })
  } catch (error) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
  const attachment: RemoteImImageAttachment = {
    type: 'image',
    localPath: cached.localPath,
    remoteUrl: null,
    thumbnailUrl: null,
    width: null,
    height: null,
    sizeBytes: cached.sizeBytes,
    fileName: input.fileName?.trim() || cached.fileName,
    mimeType: payload.mimeType,
    sdkImageId: null
  }
  const message = createRemoteImMessage(
    createPeerOutgoingImageMessageInput({
      projectId: input.projectId,
      config,
      toUserId: peerUserId,
      attachment,
      now: Date.now()
    })
  )
  if (!broadcastOutgoingImage(
    input.projectId,
    peerUserId,
    fileToken,
    message.id,
    input.origin ?? 'machine'
  )) {
    updateRemoteImMessageStatus(message.id, {
      status: 'failed',
      error: REMOTE_IM_ACCOUNT_CHANGING_ERROR
    })
    broadcastMessagesChanged(input.projectId)
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  scheduleOutgoingDeliveryAckTimeout(input.projectId, message.id)
  broadcastMessagesChanged(input.projectId)
  return { ok: true, toUserId: peerUserId }
}

async function sendRemoteImPeerLocalImage(
  projectId: string,
  localPath: string,
  toUserId?: string | null,
  origin: RemoteImMessageOrigin = 'machine'
): Promise<{ ok: boolean; error?: string; toUserId?: string }> {
  const config = await getRemoteImConfig(projectId)
  const cleanPath = localPath.trim()
  if (!cleanPath) return { ok: false, error: 'image path is required' }
  const peerUserId = resolvePeerUserId(config, toUserId)
  if (!peerUserId) {
    return { ok: false, error: '未配置远程 IM 联系人账号' }
  }
  const connectionError = getRemoteImSendConnectionError(await getRemoteImStatus(projectId))
  if (connectionError) {
    return { ok: false, error: connectionError }
  }
  if (!getRegisteredRemoteImRuntimeIdentity(projectId)) {
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }

  let payload: RemoteImLocalImagePayload
  let cached: CachedRemoteImImage
  try {
    payload = await loadRemoteImLocalImageForSend(cleanPath, {
      maxBytes: MAX_REMOTE_IM_IMAGE_BYTES
    })
    cached = await cacheRemoteImImageBytes({
      rootDir: rootDir(),
      projectId,
      fileName: payload.fileName,
      mimeType: payload.mimeType,
      bytes: payload.fileBytes
    })
  } catch (err) {
    return {
      ok: false,
      error: err instanceof Error ? err.message : String(err)
    }
  }

  const message = createRemoteImMessage(
    createPeerOutgoingImageMessageInput({
      projectId,
      config,
      toUserId: peerUserId,
      attachment: {
        ...payload.attachment,
        localPath: cached.localPath,
        sizeBytes: cached.sizeBytes,
        fileName: cached.fileName,
        mimeType: cached.mimeType
      },
      now: Date.now()
    })
  )
  if (!broadcastOutgoingImagePayload(projectId, peerUserId, payload, message.id, origin)) {
    updateRemoteImMessageStatus(message.id, {
      status: 'failed',
      error: REMOTE_IM_ACCOUNT_CHANGING_ERROR
    })
    broadcastMessagesChanged(projectId)
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  scheduleOutgoingDeliveryAckTimeout(projectId, message.id)
  broadcastMessagesChanged(projectId)
  return { ok: true, toUserId: peerUserId }
}

async function sendRemoteImPeerLocalFile(
  projectId: string,
  localPath: string,
  toUserId?: string | null,
  origin: RemoteImMessageOrigin = 'machine'
): Promise<{ ok: boolean; error?: string; toUserId?: string }> {
  const config = await getRemoteImConfig(projectId)
  const cleanPath = localPath.trim()
  if (!cleanPath) return { ok: false, error: 'file path is required' }
  const peerUserId = resolvePeerUserId(config, toUserId)
  if (!peerUserId) {
    return { ok: false, error: '未配置远程 IM 联系人账号' }
  }
  const connectionError = getRemoteImSendConnectionError(await getRemoteImStatus(projectId))
  if (connectionError) {
    return { ok: false, error: connectionError }
  }
  if (!getRegisteredRemoteImRuntimeIdentity(projectId)) {
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }

  let payload: RemoteImLocalFilePayload
  try {
    payload = await loadRemoteImLocalFileForSend(cleanPath, {
      maxBytes: MAX_REMOTE_IM_FILE_BYTES
    })
  } catch (err) {
    return {
      ok: false,
      error: err instanceof Error ? err.message : String(err)
    }
  }

  const message = createRemoteImMessage(
    createPeerOutgoingFileMessageInput({
      projectId,
      config,
      toUserId: peerUserId,
      attachment: payload.attachment,
      now: Date.now()
    })
  )
  if (!broadcastOutgoingFilePayload(projectId, peerUserId, payload, message.id, origin)) {
    updateRemoteImMessageStatus(message.id, {
      status: 'failed',
      error: REMOTE_IM_ACCOUNT_CHANGING_ERROR
    })
    broadcastMessagesChanged(projectId)
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  scheduleOutgoingDeliveryAckTimeout(projectId, message.id)
  broadcastMessagesChanged(projectId)
  return { ok: true, toUserId: peerUserId }
}

async function sendRemoteImPeerLocalVideo(
  projectId: string,
  localPath: string,
  toUserId?: string | null,
  origin: RemoteImMessageOrigin = 'machine'
): Promise<{ ok: boolean; error?: string; toUserId?: string }> {
  const config = await getRemoteImConfig(projectId)
  const cleanPath = localPath.trim()
  if (!cleanPath) return { ok: false, error: 'video path is required' }
  const peerUserId = resolvePeerUserId(config, toUserId)
  if (!peerUserId) {
    return { ok: false, error: '未配置远程 IM 联系人账号' }
  }
  const connectionError = getRemoteImSendConnectionError(await getRemoteImStatus(projectId))
  if (connectionError) {
    return { ok: false, error: connectionError }
  }
  if (!getRegisteredRemoteImRuntimeIdentity(projectId)) {
    return { ok: false, error: 'Remote IM runtime is not connected' }
  }

  let payload: RemoteImLocalVideoPayload
  try {
    payload = await loadRemoteImLocalVideoForSend(cleanPath, {
      maxBytes: MAX_REMOTE_IM_VIDEO_BYTES
    })
  } catch (err) {
    return {
      ok: false,
      error: err instanceof Error ? err.message : String(err)
    }
  }

  const message = createRemoteImMessage(
    createPeerOutgoingVideoMessageInput({
      projectId,
      config,
      toUserId: peerUserId,
      attachment: payload.attachment,
      now: Date.now()
    })
  )
  if (!broadcastOutgoingVideoPayload(projectId, peerUserId, payload, message.id, origin)) {
    updateRemoteImMessageStatus(message.id, {
      status: 'failed',
      error: REMOTE_IM_ACCOUNT_CHANGING_ERROR
    })
    broadcastMessagesChanged(projectId)
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  scheduleOutgoingDeliveryAckTimeout(
    projectId,
    message.id,
    OUTGOING_VIDEO_DELIVERY_ACK_TIMEOUT_MS
  )
  broadcastMessagesChanged(projectId)
  return { ok: true, toUserId: peerUserId }
}

async function readRemoteImFilePreview(input: {
  localPath?: string | null
  mimeType?: string | null
}): Promise<
  | { ok: true; value: { content: string; mimeType: string; fileName: string } }
  | { ok: false; error: string }
> {
  const localPath = input.localPath?.trim()
  if (!localPath) return { ok: false, error: '文件暂不可预览' }
  const mimeType = input.mimeType?.trim() || mimeTypeFromRemoteImFilePath(localPath)
  if (mimeType !== 'text/markdown' && mimeType !== 'text/html') {
    return { ok: false, error: 'unsupported file type' }
  }
  try {
    const stat = await fs.stat(localPath)
    if (!stat.isFile()) return { ok: false, error: 'file path is not a file' }
    if (stat.size > MAX_REMOTE_IM_DOC_PREVIEW_BYTES)
      return { ok: false, error: 'file is too large' }
    const content = await fs.readFile(localPath, 'utf8')
    return {
      ok: true,
      value: {
        content,
        mimeType,
        fileName: basename(localPath)
      }
    }
  } catch (err) {
    return {
      ok: false,
      error: err instanceof Error ? err.message : String(err)
    }
  }
}

function readRemoteImTranscriptReply(
  source: NonNullable<RemoteImOutputSessionState['transcript']>
) {
  if (source.kind === 'claude') {
    return readLatestClaudeRemoteImReply({
      cwd: source.cwd,
      sinceMs: source.sinceMs,
      replyId: source.replyId,
      pendingReplyIds: source.pendingReplyIds
    })
  }
  return null
}

function startOutputForwarding(
  sessionId: string,
  projectId: string,
  toUserId: string,
  config: RemoteImConfig,
  replyId: string | undefined,
  taskId: string,
  autoReplyToIm: boolean
): void {
  const runtime = getSessionRuntimeInfo(sessionId)
  const sourceKind = runtime ? getRemoteImAicliOutputSourceKind(runtime.command) : 'unknown'
  if (sourceKind === 'codex' || sourceKind === 'opencode') {
    const existing = structuredOutputTasks.resolve(
      sessionId,
      { taskId, replyId },
      { allowSoleFallback: false }
    )
    if (existing) {
      writeStructuredOutputRuntimeLog('aicli:route-steered', {
        sessionId,
        state: existing,
        detail: { inputUserId: toUserId, ownerUserId: existing.toUserId }
      })
      return
    }
    const state: RemoteImStructuredTaskState = {
      projectId,
      toUserId,
      config,
      replyId,
      taskId,
      sourceKind,
      securityGeneration: remoteImAccountSecurityGeneration,
      buffer: '',
      timer: null,
      structuredOutput: true,
      autoReplyToIm,
      sourceStarted: false
    }
    structuredOutputTasks.add(sessionId, state)
    writeStructuredOutputRuntimeLog('aicli:route-registered', {
      sessionId,
      state,
      detail: {
        activeRouteCount: structuredOutputTasks.list(sessionId).length
      }
    })
    return
  }

  if (!replyId) return

  const current = outputSessions.get(sessionId)
  if (current) {
    reserveRemoteImClaudeReplyId(current, replyId)
    writeStructuredOutputRuntimeLog('aicli:route-steered', {
      sessionId,
      state: current,
      detail: { inputUserId: toUserId, ownerUserId: current.toUserId }
    })
    return
  }
  outputSessions.set(sessionId, {
    projectId,
    toUserId,
    config,
    replyId,
    taskId,
    sourceKind,
    securityGeneration: remoteImAccountSecurityGeneration,
    buffer: '',
    timer: null,
    structuredOutput: false,
    autoReplyToIm,
    transcript:
      sourceKind === 'claude' && runtime
        ? {
            kind: sourceKind,
            cwd: runtime.targetRepo,
            sinceMs: Date.now(),
            replyId,
            pendingReplyIds: [replyId]
          }
        : undefined,
    ...(sourceKind === 'claude' ? { pendingReplyIds: [replyId] } : {})
  })
}

function rollbackClaudeOutputReservation(
  sessionId: string,
  replyId: string | undefined,
  taskId: string
): void {
  if (!replyId) return
  const state = outputSessions.get(sessionId)
  if (
    !state ||
    state.sourceKind !== 'claude' ||
    state.taskId !== taskId ||
    !state.pendingReplyIds?.includes(replyId)
  ) {
    return
  }
  rollbackRemoteImClaudeReplyId(state, replyId)
  writeStructuredOutputRuntimeLog('aicli:claude-input-reservation-rolled-back', {
    sessionId,
    state,
    detail: { rejectedReplyId: replyId, pendingReplyIds: state.pendingReplyIds }
  })
  if (state.pendingReplyIds.length > 0) return
  if (state.timer) clearTimeout(state.timer)
  if (outputSessions.get(sessionId) === state) outputSessions.delete(sessionId)
}

function bindRemoteImCliSessionToSecurityGeneration(
  sessionId: string,
  securityGeneration: number = remoteImAccountSecurityGeneration
): void {
  revokedRemoteImCliSessions.delete(sessionId)
  remoteImCliSessionSecurityGenerations.set(sessionId, securityGeneration)
}

function authorizeRemoteImCliCaller(
  projectId: string,
  sessionId: string,
  securityGeneration: number
): { ok: true } | { ok: false; error: string } {
  if (
    remoteImAccountTransitioning ||
    remoteImApprovalAuthorityMutationCount > 0 ||
    securityGeneration !== remoteImAccountSecurityGeneration
  ) {
    return { ok: false, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
  }
  const runtime = getSessionRuntimeInfo(sessionId)
  if (!runtime || runtime.projectId !== projectId) {
    return { ok: false, error: 'AICLI session is not active for this project' }
  }
  if (revokedRemoteImCliSessions.has(sessionId)) {
    return {
      ok: false,
      error: 'Remote IM authority for this AICLI task was revoked; wait for a new task or restart the session'
    }
  }
  const boundGeneration = remoteImCliSessionSecurityGenerations.get(sessionId)
  if (boundGeneration === undefined) {
    bindRemoteImCliSessionToSecurityGeneration(sessionId, securityGeneration)
    return { ok: true }
  }
  if (boundGeneration !== securityGeneration) {
    return {
      ok: false,
      error: 'Remote IM account connection changed; restart the AICLI session before using imcli'
    }
  }
  return { ok: true }
}

async function withAuthorizedRemoteImCliCaller<T>(
  projectId: string,
  sessionId: string,
  operation: () => Promise<T>
): Promise<T> {
  const result = await withRemoteImAccountBoundOperation(async (securityGeneration) => {
    const authorization = authorizeRemoteImCliCaller(
      projectId,
      sessionId,
      securityGeneration
    )
    if (!authorization.ok) {
      return { kind: 'authorization-error' as const, error: authorization.error }
    }
    return { kind: 'value' as const, value: await operation() }
  })
  if (!('kind' in result)) throw new Error(result.error)
  if (result.kind === 'authorization-error') throw new Error(result.error)
  return result.value
}

function cancelOutputForwarding(
  sessionId: string,
  replyId: string | undefined,
  taskId: string
): void {
  const structured = structuredOutputTasks.resolve(
    sessionId,
    { taskId, replyId },
    { allowSoleFallback: false }
  )
  if (structured) {
    if (structured.timer) clearTimeout(structured.timer)
    structuredOutputTasks.remove(sessionId, structured.taskId)
    writeStructuredOutputRuntimeLog('aicli:route-cancelled', {
      sessionId,
      state: structured
    })
    return
  }
  const state = outputSessions.get(sessionId)
  if (
    state?.sourceKind === 'claude' &&
    state.taskId === taskId &&
    replyId &&
    state.pendingReplyIds?.includes(replyId)
  ) {
    // The initial submission can fail while a concurrently accepted
    // continuation already owns another pending id. Roll back only the failed
    // reservation; deleting the stable owner would orphan that continuation.
    rollbackClaudeOutputReservation(sessionId, replyId, taskId)
    return
  }
  if (!state || state.replyId !== replyId) return
  if (state.timer) clearTimeout(state.timer)
  outputSessions.delete(sessionId)
}

function createOutputRoutingDeps(
  config: RemoteImConfig,
  securityGeneration = remoteImAccountSecurityGeneration
) {
  return {
    authorizeAicliOutputStart: (route: RemoteImAicliOutputRoute) => {
      if (!isRemoteImAccountSecurityGenerationCurrent(securityGeneration)) {
        return {
          ok: false as const,
          error: REMOTE_IM_ACCOUNT_CHANGING_ERROR
        }
      }
      const runtime = getSessionRuntimeInfo(route.sessionId)
      const sourceKind = runtime
        ? getRemoteImAicliOutputSourceKind(runtime.command)
        : 'unknown'
      if (sourceKind !== 'codex' && sourceKind !== 'opencode') {
        const current = outputSessions.get(route.sessionId)
        if (!current) return { ok: true as const }
        if (
          current.authorityRevoked ||
          current.securityGeneration !== remoteImAccountSecurityGeneration
        ) {
          return {
            ok: false as const,
            error: '上一条远程任务的权限已经失效，请重启 AICLI 会话后重试。'
          }
        }
        // Each Claude submission keeps its fresh reply id. The stable owner
        // state tracks all pending ids and the transcript decides whether a
        // continuation steered the current turn or became a queued next turn.
        route.taskId = current.taskId ?? route.taskId
        route.continuation = true
        return { ok: true as const }
      }

      const existingRoutes = structuredOutputTasks.list(route.sessionId)
      if (existingRoutes.length === 0) return { ok: true as const }
      const active = resolveStructuredOutputTask(route.sessionId, {})
      if (!active) {
        return {
          ok: false as const,
          error: '当前 AICLI 会话存在多个无法区分的活动任务，请先结束或重启会话。'
        }
      }
      if (
        active.authorityRevoked ||
        active.securityGeneration !== remoteImAccountSecurityGeneration
      ) {
        return {
          ok: false as const,
          error: '上一条远程任务的权限已经失效，请重启 AICLI 会话后重试。'
        }
      }
      route.replyId = active.replyId
      route.taskId = active.taskId
      route.continuation = true
      return { ok: true as const }
    },
    onAicliOutputStart: ({
      sessionId,
      projectId,
      toUserId,
      replyId,
      taskId,
      autoReplyToIm
    }: RemoteImAicliOutputRoute) =>
      startOutputForwarding(
        sessionId,
        projectId,
        toUserId,
        config,
        replyId,
        taskId,
        autoReplyToIm
      ),
    onAicliInputAccepted: (route: RemoteImAicliOutputRoute) => {
      bindRemoteImCliSessionToSecurityGeneration(route.sessionId)
      const runtime = getSessionRuntimeInfo(route.sessionId)
      const sourceKind = runtime
        ? getRemoteImAicliOutputSourceKind(runtime.command)
        : 'unknown'
      if (
        route.continuation &&
        (sourceKind === 'codex' || sourceKind === 'opencode')
      ) {
        structuredOutputTasks.clearLocalTakeover(route.sessionId, route.taskId)
        startOutputForwarding(
          route.sessionId,
          route.projectId,
          route.toUserId,
          config,
          route.replyId,
          route.taskId,
          route.autoReplyToIm
        )
      }
    },
    onAicliInputRejected: ({ sessionId, replyId, taskId }: RemoteImAicliOutputRoute) =>
      rollbackClaudeOutputReservation(sessionId, replyId, taskId),
    onAicliMachineInputAccepted: (sessionId: string) =>
      bindRemoteImCliSessionToSecurityGeneration(sessionId),
    onAicliOutputCancel: ({ sessionId, replyId, taskId }: RemoteImAicliOutputRoute) =>
      cancelOutputForwarding(sessionId, replyId, taskId),
    messagesChanged: broadcastMessagesChanged
  }
}

function outputForwardingDeps(expectedSecurityGeneration: number) {
  const failDelivery = (
    delivery: PendingRemoteImOutputDelivery | null,
    error: string
  ): void => {
    if (!delivery) return
    if (pendingRemoteImOutputDeliveries.get(delivery.messageId) !== delivery) return
    pendingRemoteImOutputDeliveries.delete(delivery.messageId)
    // A message id is scoped to the active account database. Never apply an
    // old delivery failure after the account generation has changed: the new
    // account may already have reused the same integer id.
    if (delivery.securityGeneration !== remoteImAccountSecurityGeneration) return
    failRemoteImMessageIfStreaming(delivery.messageId, error)
    broadcastMessagesChanged(delivery.projectId)
  }

  const sendTextWhenAuthorityIsStable = async (
    projectId: string,
    toUserId: string,
    text: string,
    messageId: number | undefined,
    delivery: PendingRemoteImOutputDelivery | null
  ): Promise<void> => {
    for (;;) {
      if (expectedSecurityGeneration !== remoteImAccountSecurityGeneration) {
        // Account invalidation cancels and marks the old database row before
        // switching stores. This delayed queue entry must now be a no-op.
        return
      }
      if (remoteImAccountTransitioning) {
        failDelivery(delivery, REMOTE_IM_ACCOUNT_CHANGING_ERROR)
        return
      }
      if (remoteImApprovalAuthorityMutationCount > 0) {
        const pendingMutation = remoteImAccountBindQueue
        await pendingMutation
        continue
      }
      const result = await withRemoteImAccountBoundOperation(async () => {
        if (expectedSecurityGeneration !== remoteImAccountSecurityGeneration) {
          return { ok: false as const, error: REMOTE_IM_ACCOUNT_CHANGING_ERROR }
        }
        const config = await getRemoteImConfig(projectId)
        if (resolvePeerUserId(config, toUserId) !== toUserId) {
          return { ok: false as const, error: 'Remote IM contact authority was revoked' }
        }
        if (!broadcastOutgoingText(projectId, toUserId, text, messageId)) {
          return { ok: false as const, error: 'Remote IM runtime is not connected' }
        }
        if (messageId) scheduleOutgoingDeliveryAckTimeout(projectId, messageId)
        return { ok: true as const }
      })
      if (!result.ok) {
        failDelivery(delivery, result.error)
      } else if (delivery) {
        pendingRemoteImOutputDeliveries.delete(delivery.messageId)
      }
      return
    }
  }

  return {
    createMessage: (input: Parameters<typeof createRemoteImMessage>[0]) => {
      // A provider terminal may arrive after account invalidation. Never put
      // content owned by the old route into the newly selected account store.
      if (
        remoteImAccountTransitioning ||
        expectedSecurityGeneration !== remoteImAccountSecurityGeneration
      ) {
        return undefined
      }
      return createRemoteImMessage(input)
    },
    sendText: (
      projectId: string,
      toUserId: string,
      text: string,
      messageId?: number
    ) => {
      if (expectedSecurityGeneration !== remoteImAccountSecurityGeneration) return
      const pendingDelivery = messageId
        ? {
            projectId,
            messageId,
            securityGeneration: expectedSecurityGeneration
          }
        : null
      if (pendingDelivery) {
        pendingRemoteImOutputDeliveries.set(messageId!, pendingDelivery)
      }
      const delivery = remoteImOutputDeliveryQueue.then(() =>
        sendTextWhenAuthorityIsStable(
          projectId,
          toUserId,
          text,
          messageId,
          pendingDelivery
        )
      )
      remoteImOutputDeliveryQueue = delivery.catch(() => undefined)
      void delivery.catch((err) => {
        failDelivery(pendingDelivery, err instanceof Error ? err.message : String(err))
      })
    },
    messagesChanged: broadcastMessagesChanged,
    readTranscriptReply: readRemoteImTranscriptReply
  }
}

function resolveStructuredOutputTask(
  sessionId: string,
  identity: { taskId?: string; replyId?: string }
): RemoteImStructuredTaskState | undefined {
  const tasks = structuredOutputTasks.list(sessionId)
  if (identity.taskId || identity.replyId) {
    // Explicit correlation is authoritative. A stale or forged id must not be
    // redirected to whichever source route happens to be the only one alive.
    return structuredOutputTasks.resolve(sessionId, identity, {
      allowSoleFallback: false
    })
  }

  const running = tasks.filter((task) => task.sourceStarted)
  if (running.length === 1) return running[0]
  if (running.length === 0 && tasks.length === 1) return tasks[0]
  return undefined
}

function removeStructuredOutputTask(
  sessionId: string,
  state: RemoteImStructuredTaskState,
  reason: string
): void {
  if (state.timer) clearTimeout(state.timer)
  state.timer = null
  structuredOutputTasks.remove(sessionId, state.taskId)
  writeStructuredOutputRuntimeLog('aicli:route-removed', {
    sessionId,
    state,
    detail: { reason }
  })
}

function markStructuredTaskActive(state: RemoteImStructuredTaskState): void {
  state.sourceStarted = true
  state.lastActivityAt = Date.now()
}

function scheduleClaudeTranscriptCompletionPoll(
  sessionId: string,
  state: RemoteImAccountBoundOutputSessionState
): void {
  if (
    outputSessions.get(sessionId) !== state ||
    state.sourceKind !== 'claude' ||
    !state.awaitingTranscriptCompletion ||
    state.timer
  ) {
    return
  }
  const polls = state.transcriptCompletionPolls ?? 0
  if (polls >= MAX_CLAUDE_TRANSCRIPT_COMPLETION_POLLS) {
    state.awaitingTranscriptCompletion = false
    writeStructuredOutputRuntimeLog('aicli:claude-transcript-frame-timeout', {
      sessionId,
      state,
      detail: {
        replyId: state.replyId ?? null,
        polls
      }
    })
    return
  }
  state.transcriptCompletionPolls = polls + 1
  state.timer = setTimeout(
    () => {
      if (outputSessions.get(sessionId) !== state) return
      flushOutputSession(sessionId)
    },
    CLAUDE_TRANSCRIPT_COMPLETION_POLL_MS
  )
  state.timer.unref?.()
}

function flushOutputSession(sessionId: string): void {
  const state = outputSessions.get(sessionId)
  if (!state) return
  const forwardedChunks = flushRemoteImOutputSession(
    sessionId,
    state,
    outputForwardingDeps(state.securityGeneration)
  )
  if (
    outputSessions.get(sessionId) === state &&
    isRemoteImClaudeRouteConsumed(state)
  ) {
    outputSessions.delete(sessionId)
    writeStructuredOutputRuntimeLog('aicli:claude-reply-frame-consumed', {
      sessionId,
      state,
      detail: {
        replyId: state.replyId,
        forwardedChunks,
        autoReplyToIm: state.autoReplyToIm !== false
      }
    })
    return
  }
  if (
    outputSessions.get(sessionId) === state &&
    state.sourceKind === 'claude' &&
    state.awaitingTranscriptCompletion
  ) {
    scheduleClaudeTranscriptCompletionPoll(sessionId, state)
  }
}

function completeOutputSession(sessionId: string, info: RemoteImOutputCompletionInfo = {}): void {
  const state = outputSessions.get(sessionId)
  if (!state) return
  completeRemoteImOutputSession(
    sessionId,
    state,
    outputForwardingDeps(state.securityGeneration),
    info
  )
  outputSessions.delete(sessionId)
}

function failOutputSession(sessionId: string, reason: string): void {
  const state = outputSessions.get(sessionId)
  if (!state) return
  failRemoteImOutputSession(
    sessionId,
    state,
    outputForwardingDeps(state.securityGeneration),
    reason
  )
  outputSessions.delete(sessionId)
}

function scheduleOutputFlush(sessionId: string): void {
  const state = outputSessions.get(sessionId)
  if (!state || state.timer) return
  state.timer = setTimeout(() => {
    if (outputSessions.get(sessionId) !== state) return
    flushOutputSession(sessionId)
  }, state.config.outputFlushIntervalMs)
}

let sessionListenersRegistered = false
let remoteImCliServerStarted = false

function ensureSessionListeners(): void {
  if (sessionListenersRegistered) return
  sessionListenersRegistered = true
  addSessionDataListener(({ sessionId, chunk }) => {
    const state = outputSessions.get(sessionId)
    if (!state) return
    if (state.structuredOutput) return
    state.buffer += chunk
    scheduleOutputFlush(sessionId)
  })
  addSessionLocalInputListener(({ sessionId, kind }) => {
    const state = outputSessions.get(sessionId)
    if (state && kind !== 'navigation') {
      if (state.timer) clearTimeout(state.timer)
      state.timer = null
      state.buffer = ''
      state.autoReplyToIm = false
      outputSessions.delete(sessionId)
      void getRemoteImApprovalCoordinator().cancelSession(sessionId)
      writeStructuredOutputRuntimeLog('aicli:route-deactivated', {
        sessionId,
        state,
        detail: {
          origin: 'local',
          admissionLockRetained: false,
          sourceKind: state.sourceKind ?? 'unknown'
        }
      })
    }
  })
  addAicliStructuredOutputListener((event) => {
    const {
      sessionId,
      kind,
      text,
      messageId,
      partId,
      replyId,
      taskId,
      approvalId,
      threadId,
      turnId,
      cwd,
      reason,
      persistentApprovalCommand
    } = event
    if (kind === 'input_origin') {
      const origin = text.trim()
      const routes = structuredOutputTasks.list(sessionId)
      if (origin === 'remote-im') {
        for (const state of routes) {
          writeStructuredOutputRuntimeLog('aicli:route-origin', {
            sessionId,
            state,
            detail: { origin }
          })
        }
        return
      }
      // Local input can steer the still-running Codex turn. Stop forwarding
      // that turn, but retain every route as an admission tombstone until its
      // real terminal event. Releasing it here would let a different friend
      // enter the same turn while Rust still attributes approvals to the old
      // immutable remote route.
      const retainedRoutes = structuredOutputTasks.markLocalTakeover(sessionId)
      void getRemoteImApprovalCoordinator().cancelSession(sessionId)
      for (const state of retainedRoutes) {
        writeStructuredOutputRuntimeLog('aicli:route-deactivated', {
          sessionId,
          state,
          detail: {
            origin: origin || 'local',
            admissionLockRetained: true
          }
        })
      }
      return
    }

    if (kind === 'approval_request') {
      // Approval requests must carry their task identity. No sole-route or
      // "currently active user" fallback is allowed for a destructive action.
      const state = taskId && replyId
        ? structuredOutputTasks.resolve(
            sessionId,
            { taskId, replyId },
            { allowSoleFallback: false }
          )
        : undefined
      const detail = {
        kind,
        messageId: messageId ?? null,
        taskId: taskId ?? null,
        replyId: replyId ?? null,
        threadId: threadId ?? null,
        turnId: turnId ?? null,
        approvalId: approvalId ?? null,
        commandLength: text.length
      }
      if (
        !state ||
        !taskId ||
        !replyId ||
        !threadId ||
        !turnId ||
        !approvalId ||
        !cwd ||
        !text.trim() ||
        (taskId ? structuredOutputTasks.isLocallyTakenOver(sessionId, taskId) : false)
      ) {
        // Privacy: command text may contain paths, arguments or credentials.
        // Approval logs intentionally record only its length and correlation.
        writeStructuredOutputRuntimeLog('aicli:approval-unmatched', {
          sessionId,
          detail: {
            ...detail,
            candidates: structuredOutputTasks.list(sessionId).map((candidate) => ({
              taskId: candidate.taskId,
              replyId: candidate.replyId ?? null
            }))
          }
        })
        // A complete approval identity is sufficient to reject the request
        // safely even when its remote-IM route is missing or the payload is
        // malformed. Never leave Codex waiting indefinitely and never guess a
        // requester from another live route.
        if (taskId && threadId && turnId && approvalId) {
          void requestAicliApprovalForSession(sessionId, {
            taskId,
            threadId,
            turnId,
            approvalId,
            decision: 'cancel'
          }).then((result) => {
            writeStructuredOutputRuntimeLog('aicli:approval-unmatched-cancelled', {
              sessionId,
              detail: {
                taskId,
                replyId,
                threadId,
                turnId,
                approvalId,
                ok: result.ok,
                error: result.ok ? null : result.error
              }
            })
          })
        }
        return
      }
      writeStructuredOutputRuntimeLog('aicli:approval-received', {
        sessionId,
        state,
        detail
      })
      void getRemoteImApprovalCoordinator()
        .register({
          projectId: state.projectId,
          requesterUserId: state.toUserId,
          sessionId,
          taskId,
          replyId,
          threadId,
          turnId,
          approvalId,
          commandText: text,
          cwd,
          ...(reason ? { reason } : {}),
          ...(persistentApprovalCommand ? { persistentApprovalCommand } : {})
        })
        .then((result) => {
          writeStructuredOutputRuntimeLog(
            result.ok ? 'aicli:approval-forwarded' : 'aicli:approval-forward-failed',
            {
              sessionId,
              state,
              detail: {
                taskId,
                replyId,
                threadId,
                turnId,
                approvalId,
                commandLength: text.length,
                ok: result.ok,
                error: result.error ?? null
              }
            }
          )
        })
      return
    }

    if (kind === 'approval_resolved') {
      if (taskId && replyId && threadId && turnId && approvalId) {
        getRemoteImApprovalCoordinator().forgetResolved({
          sessionId,
          taskId,
          replyId,
          threadId,
          turnId,
          approvalId
        })
      }
      writeStructuredOutputRuntimeLog('aicli:approval-resolved', {
        sessionId,
        detail: {
          taskId: taskId ?? null,
          replyId: replyId ?? null,
          threadId: threadId ?? null,
          turnId: turnId ?? null,
          approvalId: approvalId ?? null
        }
      })
      return
    }

    const state = resolveStructuredOutputTask(sessionId, { replyId, taskId })
    if (!state) {
      writeStructuredOutputRuntimeLog('aicli:event-unmatched', {
        sessionId,
        detail: {
          kind,
          messageId: messageId ?? null,
          replyId: replyId ?? null,
          taskId: taskId ?? null,
          textLength: text.length,
          textPreview: structuredOutputTextPreview(text),
          candidates: structuredOutputTasks.list(sessionId).map((candidate) => ({
            taskId: candidate.taskId,
            replyId: candidate.replyId ?? null,
            sourceStarted: candidate.sourceStarted ?? false
          }))
        }
      })
      return
    }
    if (structuredOutputTasks.isLocallyTakenOver(sessionId, state.taskId)) {
      writeStructuredOutputRuntimeLog('aicli:event-ignored-after-local-takeover', {
        sessionId,
        state,
        detail: {
          kind,
          messageId: messageId ?? null,
          eventReplyId: replyId ?? null,
          eventTaskId: taskId ?? null
        }
      })
      if (kind === 'assistant_final' || kind === 'turn_error') {
        removeStructuredOutputTask(sessionId, state, `local-takeover-${kind}`)
      }
      return
    }
    writeStructuredOutputRuntimeLog('aicli:event-received', {
      sessionId,
      state,
      detail: {
        kind,
        messageId: messageId ?? null,
        eventReplyId: replyId ?? null,
        eventTaskId: taskId ?? null,
        textLength: text.length,
        textPreview: structuredOutputTextPreview(text)
      }
    })
    if (kind === 'task_started') {
      if (!state.sourceStarted) state.forwardedStructuredAssistantTexts = []
      markStructuredTaskActive(state)
      return
    }
    if (kind === 'task_activity') {
      markStructuredTaskActive(state)
      return
    }
    if (kind === 'turn_error') {
      state.buffer = ''
      failRemoteImOutputSession(
        sessionId,
        state,
        outputForwardingDeps(state.securityGeneration),
        text
      )
      writeStructuredOutputRuntimeLog('aicli:error-forwarded', {
        sessionId,
        state,
        detail: {
          messageId: messageId ?? null,
          reasonPreview: structuredOutputTextPreview(text)
        }
      })
      removeStructuredOutputTask(sessionId, state, 'turn-error')
      return
    }
    if (kind === 'assistant_final') {
      const resolved = resolveRemoteImStructuredFinalContent(text, state.replyId)
      const hadImmediateAssistantOutput =
        (state.forwardedStructuredAssistantTexts?.length ?? 0) > 0
      const forwardedChunks = forwardRemoteImStructuredFinalOutput(
        sessionId,
        state,
        outputForwardingDeps(state.securityGeneration),
        text,
        messageId
      )
      const finalEvent =
        state.autoReplyToIm === false
          ? 'aicli:final-suppressed'
          : forwardedChunks > 0
          ? 'aicli:final-forwarded'
          : hadImmediateAssistantOutput
            ? 'aicli:final-already-forwarded'
            : 'aicli:final-empty'
      writeStructuredOutputRuntimeLog(
        finalEvent,
        {
          sessionId,
          state,
          detail: {
            messageId: messageId ?? null,
            eventReplyId: replyId ?? null,
            eventTaskId: taskId ?? null,
            contentSource: resolved.source,
            markerReplyIdMismatch: resolved.markerReplyIdMismatch,
            inputLength: text.length,
            resolvedLength: resolved.content.length,
            forwardedChunks
          }
        }
      )
      if (
        state.autoReplyToIm !== false &&
        forwardedChunks === 0 &&
        !hadImmediateAssistantOutput
      ) {
        failRemoteImOutputSession(
          sessionId,
          state,
          outputForwardingDeps(state.securityGeneration),
          '任务已经结束，但最终回复为空或无法解析。请查看 remote-im-runtime.log。'
        )
      }
      removeStructuredOutputTask(sessionId, state, 'assistant-final')
      return
    }
    if (kind && kind !== 'assistant_text') return
    markStructuredTaskActive(state)
    const forwardedChunks = forwardRemoteImStructuredAssistantOutput(
      sessionId,
      state,
      outputForwardingDeps(state.securityGeneration),
      text,
      messageId,
      partId
    )
    writeStructuredOutputRuntimeLog(
      forwardedChunks > 0 ? 'aicli:assistant-forwarded' : 'aicli:assistant-skipped',
      {
        sessionId,
        state,
        detail: {
          messageId: messageId ?? null,
          partId: partId ?? null,
          eventReplyId: replyId ?? null,
          eventTaskId: taskId ?? null,
          inputLength: text.length,
          forwardedChunks
        }
      }
    )
  })
  addSessionExitListener(({ sessionId, exitCode, signal }) => {
    remoteImCliSessionSecurityGenerations.delete(sessionId)
    revokedRemoteImCliSessions.delete(sessionId)
    void getRemoteImApprovalCoordinator().cancelSession(sessionId)
    const reason = signal
      ? `AICLI 进程已退出（信号：${signal}）。`
      : typeof exitCode === 'number'
        ? `AICLI 进程已退出（退出码：${exitCode}）。`
        : 'AICLI 进程已退出。'
    for (const state of structuredOutputTasks.list(sessionId)) {
      state.buffer = ''
      if (!structuredOutputTasks.isLocallyTakenOver(sessionId, state.taskId)) {
        failRemoteImOutputSession(
          sessionId,
          state,
          outputForwardingDeps(state.securityGeneration),
          reason
        )
      }
      removeStructuredOutputTask(sessionId, state, 'session-exit')
    }
    completeOutputSession(sessionId, { exitCode, signal })
  })
}

function ensureRemoteImCliServer(): void {
  if (remoteImCliServerStarted) return
  remoteImCliServerStarted = true
  void startRemoteImCliServer({
    rootDir: rootDir(),
    getConfig: getRemoteImConfig,
    getStatus: getRemoteImStatus,
    listMessages: listRemoteImMessages,
    withAuthorizedCaller: withAuthorizedRemoteImCliCaller,
    sendPeerMessage: (projectId, text, toUserId) =>
      sendRemoteImPeerMessage(projectId, text, toUserId, 'machine'),
    sendPeerImage: (projectId, localPath, toUserId) =>
      sendRemoteImPeerLocalImage(projectId, localPath, toUserId, 'machine'),
    sendPeerFile: (projectId, localPath, toUserId) =>
      sendRemoteImPeerLocalFile(projectId, localPath, toUserId, 'machine'),
    sendPeerVideo: (projectId, localPath, toUserId) =>
      sendRemoteImPeerLocalVideo(projectId, localPath, toUserId, 'machine'),
    addContact: (projectId, userId) => addRemoteImContact(projectId, userId)
  }).catch((err) => {
    remoteImCliServerStarted = false
    console.error(
      '[remote-im] failed to start imcli bridge:',
      err instanceof Error ? err.message : String(err)
    )
  })
}

export interface RegisterRemoteImIpcOptions {
  /**
   * 账号绑定成功后初始化账号作用域数据层（rootDir/DB/单实例锁/后台服务）。
   * 返回 alreadyLocked 表示该账号已在另一个窗口打开。
   */
  activateDataLayer?: (
    userId: string
  ) => Promise<{ ok: true } | { ok: false; alreadyLocked?: boolean; error?: string }>
}

/** 账号绑定后启动 imcli 桥接服务（依赖账号作用域 rootDir，不能在登录前跑）。 */
export function activateRemoteImDataLayer(): void {
  ensureRemoteImCliServer()
}

/**
 * 绑定 IM 账号配置：切 active profile、写账号配置、连接变更时重置状态。set-account 与
 * bind-account 共用。注意：依赖账号作用域 rootDir，调用前须已 setActiveAccount。
 */
async function bindRemoteImAccountConfig(
  account: RemoteImAccountConfig
): Promise<{ profileId: string; account: RemoteImAccountConfig }> {
  return enqueueRemoteImApprovalAuthorityMutation(() =>
    bindRemoteImAccountConfigOnce(account)
  )
}

async function bindRemoteImAccountConfigOnce(
  account: RemoteImAccountConfig
): Promise<{ profileId: string; account: RemoteImAccountConfig }> {
  const hasExplicitBlockedUserIds = Array.isArray(account.blockedUserIds)
  const normalizedAccount = normalizeRemoteImAccountConfig(account)
  const activeAccountId = getActiveAccount()
  if (
    activeAccountId &&
    normalizedAccount.desktopUserId &&
    sanitizeAccountId(normalizedAccount.desktopUserId) !== activeAccountId
  ) {
    throw new Error('当前窗口已绑定另一个账号；切换账号请重启应用。')
  }
  const previousProfileId = getCurrentRemoteImAccountProfileId()
  const previousAccount = previousProfileId
    ? await readRemoteImAccountConfig(remoteImAccountDir(previousProfileId))
    : normalizeRemoteImAccountConfig(null)
  const profileId =
    getRemoteImAccountProfileId(normalizedAccount.desktopUserId) ??
    getRemoteImProfileId() ??
    DEFAULT_REMOTE_IM_PROFILE_ID
  const targetAccount = await readRemoteImAccountConfig(remoteImAccountDir(profileId))
  const nextAccount = preserveRemoteImAccountContacts(
    hasExplicitBlockedUserIds
      ? normalizedAccount
      : { ...normalizedAccount, blockedUserIds: undefined },
    targetAccount
  )
  const removedContactUserIds = removedRemoteImAccountContactUserIds(
    previousAccount,
    nextAccount
  )
  const connectionChanged =
    previousProfileId !== profileId ||
    hasRemoteImAccountConnectionChanged(previousAccount, nextAccount)
  if (connectionChanged) {
    remoteImAccountTransitioning = true
    await invalidateRemoteImSecurityStateForAccountChange()
    // All incoming handlers use account-scoped message/history/cache stores.
    // Do not switch those stores underneath a handler that started on the old
    // account; generation-aware routing makes such handlers fail closed.
    await waitForRemoteImAccountBoundOperationsToDrain()
  } else {
    await revokeRemoteImApprovalAuthorityForUsers(removedContactUserIds)
  }
  try {
    activeRemoteImAccountProfileId = profileId
    const value = await writeRemoteImAccountConfig(
      remoteImAccountDir(profileId),
      nextAccount
    )
    if (connectionChanged) resetRemoteImStatusesAfterAccountChange()
    return { profileId, account: value }
  } finally {
    if (connectionChanged) {
      remoteImAccountTransitioning = false
    }
  }
}

function getActiveRemoteImSessionForProject(
  projectId: string
): RemoteImSessionInfo | null {
  const session = getActiveSessionForProject(projectId)
  if (!session) return null
  const runtime = getSessionRuntimeInfo(session.sessionId)
  return {
    ...session,
    sourceKind: runtime ? getRemoteImAicliOutputSourceKind(runtime.command) : 'unknown'
  }
}

export function registerRemoteImIpc(options: RegisterRemoteImIpcOptions = {}): void {
  // ensureSessionListeners 只挂 PTY 输出/退出监听，登录前也安全；imcli 桥接服务
  // （ensureRemoteImCliServer）依赖账号作用域 rootDir，移到 activateRemoteImDataLayer。
  ensureSessionListeners()

  ipcMain.handle(
    'remote-im:bind-account',
    async (_event, { account }: { account: RemoteImAccountConfig }) => {
      try {
        const normalized = normalizeRemoteImAccountConfig(account)
        const userId = normalized.desktopUserId?.trim()
        if (!userId)
          return {
            ok: false as const,
            error: '请填写 IM 账号（desktopUserId）'
          }
        // 1) 用账号初始化数据层并抢单实例锁
        const activated = options.activateDataLayer
          ? await options.activateDataLayer(userId)
          : ({ ok: true } as const)
        if (!activated.ok) {
          return {
            ok: false as const,
            alreadyLocked: activated.alreadyLocked === true,
            error:
              activated.alreadyLocked === true
                ? '该账号已在另一个 Multi-AI Code 窗口打开'
                : (activated.error ?? '账号数据层初始化失败')
          }
        }
        // 2) 写账号配置（此时 rootDir 已按账号作用域就绪）
        await bindRemoteImAccountConfig(account)
        // 3) 读回登录态供渲染层解锁登录门
        return { ok: true as const, value: await getRemoteImLoginState() }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle('remote-im:get-config', async (_event, { projectId }: { projectId: string }) => {
    try {
      return { ok: true as const, value: await getRemoteImConfig(projectId) }
    } catch (err) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })

  ipcMain.handle('remote-im:get-login-state', async () => {
    try {
      return { ok: true as const, value: await getRemoteImLoginState() }
    } catch (err) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })

  ipcMain.handle(
    'remote-im:get-account-by-user-id',
    async (_event, { userId }: { userId: string }) => {
      try {
        return {
          ok: true as const,
          value: await getRemoteImAccountByUserId(userId)
        }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'remote-im:set-account',
    async (_event, { account }: { account: RemoteImAccountConfig }) => {
      try {
        return {
          ok: true as const,
          value: await bindRemoteImAccountConfig(account)
        }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'remote-im:set-config',
    async (_event, { projectId, config }: { projectId: string; config: unknown }) => {
      try {
        return await setRemoteImConfig(projectId, config)
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle('remote-im:get-status', async (_event, { projectId }: { projectId: string }) => {
    return await getRemoteImStatus(projectId)
  })

  ipcMain.handle(
    'remote-im:list-messages',
    (_event, { projectId, limit }: { projectId: string; limit?: number }) =>
      listRemoteImMessages(projectId, limit ?? 100)
  )

  // 汇总视图：一次取回项目最近的消息全集（独立于普通列表的 500 上限）。
  ipcMain.handle(
    'remote-im:list-messages-for-summary',
    (_event, { projectId, limit }: { projectId: string; limit?: number }) =>
      listRemoteImMessagesForSummary(projectId, limit ?? 3000)
  )

  // 图片预览只接受消息 id：实际路径从当前账号数据库中的可信附件记录读取，
  // 避免渲染层借 IPC 读取任意本地文件。
  ipcMain.handle(
    'remote-im:read-image-preview',
    async (_event, input: ReadRemoteImImagePreviewInput) => {
      try {
        const projectId = typeof input?.projectId === 'string' ? input.projectId.trim() : ''
        const messageId = input?.messageId
        if (!projectId || !Number.isSafeInteger(messageId) || messageId <= 0) {
          throw new Error('图片预览参数无效')
        }

        const message = listRemoteImMessageById(messageId)
        if (
          !message ||
          message.projectId !== projectId ||
          message.kind !== 'image' ||
          message.attachment?.type !== 'image' ||
          !message.attachment.localPath
        ) {
          throw new Error('图片消息不存在或不属于当前项目')
        }

        return {
          ok: true as const,
          dataUrl: await readRemoteImLocalImageDataUrl(message.attachment.localPath, {
            maxBytes: MAX_REMOTE_IM_IMAGE_BYTES,
            allowedDirectory: remoteImImageCacheDirectory(rootDir(), projectId)
          })
        }
      } catch (error) {
        return {
          ok: false as const,
          error: error instanceof Error ? error.message : String(error)
        }
      }
    }
  )

  // 消息汇总落盘为 .md 文件（发送给 AICLI 时把文件路径交给它读取）。
  ipcMain.handle(
    'remote-im:save-summary-markdown',
    async (_event, { projectId, markdown }: { projectId: string; markdown: string }) => {
      try {
        const dir = join(rootDir(), 'remote-im-summaries')
        await fs.mkdir(dir, { recursive: true })
        const now = new Date()
        const pad = (value: number) => String(value).padStart(2, '0')
        const stamp = `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`
        const safeProjectId = projectId.replace(/[^\w-]/g, '_')
        const filePath = join(dir, `messages-summary-${safeProjectId}-${stamp}.md`)
        await fs.writeFile(filePath, markdown, 'utf8')
        return { ok: true as const, path: filePath }
      } catch (cause) {
        return {
          ok: false as const,
          error: cause instanceof Error ? cause.message : '保存消息汇总文件失败'
        }
      }
    }
  )

  ipcMain.handle(
    'remote-im:delete-contact',
    async (_event, { projectId, userId }: { projectId: string; userId: string }) => {
      try {
        return await enqueueRemoteImApprovalAuthorityMutation(() =>
          deleteRemoteImContact(projectId, userId)
        )
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'remote-im:sync-contacts',
    async (
      _event,
      {
        projectId,
        userIds,
        runtimeIdentity
      }: {
        projectId: string
        userIds: string[]
        runtimeIdentity: RemoteImRuntimeIdentity
      }
    ) => {
      try {
        return await enqueueRemoteImApprovalAuthorityMutation(() =>
          syncRemoteImContactsFromSdk(
            projectId,
            Array.isArray(userIds) ? userIds : [],
            runtimeIdentity
          )
        )
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'remote-im:update-sdk-status',
    (
      _event,
      {
        status,
        runtimeIdentity
      }: {
        status: Pick<RemoteImStatus, 'projectId' | 'state' | 'detail'>
        runtimeIdentity?: RemoteImRuntimeIdentity
      }
    ) => {
      if (
        runtimeIdentity &&
        (!status.projectId || !isCurrentRemoteImRuntime(status.projectId, runtimeIdentity))
      ) {
        return { ok: false as const, error: 'Remote IM runtime is stale' }
      }
      broadcastStatus({
        projectId: status.projectId,
        state: status.state,
        detail: status.detail,
        updatedAt: Date.now()
      })
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'remote-im:mark-outgoing-message-sent',
    (
      _event,
      {
        projectId,
        messageId,
        remoteMessageId,
        runtimeIdentity
      }: {
        projectId: string
        messageId: number
        remoteMessageId?: string | null
        runtimeIdentity: RemoteImRuntimeIdentity
      }
    ) => {
      const message = listRemoteImMessageById(messageId)
      if (
        !isRegisteredRemoteImRuntime(projectId, runtimeIdentity) ||
        !message ||
        message.projectId !== projectId ||
        message.direction !== 'outgoing' ||
        message.status !== 'streaming'
      ) {
        return { ok: false as const, error: 'Remote IM delivery acknowledgement is stale' }
      }
      clearOutgoingDeliveryAckTimeout(projectId, messageId, runtimeIdentity)
      updateRemoteImMessageStatus(messageId, {
        status: 'sent-to-im',
        error: null,
        sentToImAt: Date.now(),
        // SDK 确认的消息 id 回填：漫游重投同一条消息时按 remote_message_id 去重。
        ...(remoteMessageId ? { remoteMessageId } : {})
      })
      settleApprovalDelivery(messageId, { ok: true })
      broadcastMessagesChanged(projectId)
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'remote-im:mark-outgoing-message-failed',
    (
      _event,
      {
        projectId,
        messageId,
        error,
        runtimeIdentity
      }: {
        projectId: string
        messageId: number
        error: string
        runtimeIdentity: RemoteImRuntimeIdentity
      }
    ) => {
      const message = listRemoteImMessageById(messageId)
      if (
        !isRegisteredRemoteImRuntime(projectId, runtimeIdentity) ||
        !message ||
        message.projectId !== projectId ||
        message.direction !== 'outgoing' ||
        message.status !== 'streaming'
      ) {
        return { ok: false as const, error: 'Remote IM delivery acknowledgement is stale' }
      }
      clearOutgoingDeliveryAckTimeout(projectId, messageId, runtimeIdentity)
      updateRemoteImMessageStatus(messageId, {
        status: 'failed',
        error: error || 'failed to send IM message'
      })
      settleApprovalDelivery(messageId, {
        ok: false,
        error: error || 'failed to send IM message'
      })
      broadcastMessagesChanged(projectId)
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'remote-im:write-runtime-log',
    async (_event, { entry }: { entry: RemoteImRuntimeLogEntryInput }) => {
      try {
        await appendRemoteImRuntimeLog(rootDir(), entry)
        return { ok: true as const }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'remote-im:register-runtime',
    async (
      _event,
      {
        projectId,
        runtimeIdentity
      }: { projectId: string; runtimeIdentity: RemoteImRuntimeIdentity }
    ) =>
      withRemoteImAccountBoundOperation(async (securityGeneration) => {
        const config = await getRemoteImConfig(projectId)
        if (
          !runtimeIdentity.connectionId?.trim() ||
          runtimeIdentity.desktopUserId.trim() !== config.desktopUserId.trim() ||
          runtimeIdentity.sdkAppId !== config.sdkAppId
        ) {
          return { ok: false as const, error: '远程 IM 连接身份与当前账号不匹配' }
        }
        remoteImRuntimeIdentities.set(projectId, {
          ...runtimeIdentity,
          connectionId: runtimeIdentity.connectionId.trim(),
          desktopUserId: runtimeIdentity.desktopUserId.trim(),
          securityGeneration
        })
        return { ok: true as const }
      })
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-text',
    async (
      _event,
      {
        message,
        runtimeIdentity
      }: { message: RemoteImIncomingTextMessage; runtimeIdentity: RemoteImRuntimeIdentity }
    ) =>
      withRemoteImIncomingOperation(async (securityGeneration) => {
      if (!isCurrentRemoteImRuntime(message.projectId, runtimeIdentity)) {
        return { ok: false as const, error: 'Remote IM runtime is stale' }
      }
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveRemoteImSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        ...createOutputRoutingDeps(config, securityGeneration),
        sendUser: sendUserMessageToSession,
        sendImText,
        sendImFile: (projectId, toUserId, localPath) =>
          sendRemoteImPeerLocalFile(projectId, localPath, toUserId, 'machine'),
        handleApprovalCommand: (input) =>
          getRemoteImApprovalCoordinator().handleCommand(input),
        handleControlCommand: async ({ command, args, replyId, taskId }) => {
          const runtime = session ? getSessionRuntimeInfo(session.sessionId) : null
          const sourceKind = runtime ? getRemoteImAicliOutputSourceKind(runtime.command) : 'unknown'
          return executeRemoteImControlCommand({
            command,
            sourceKind,
            session: runtime
              ? {
                  sessionId: session?.sessionId ?? '',
                  targetRepo: runtime.targetRepo,
                  command: runtime.command,
                  startedAtMs: runtime.startedAtMs
                }
              : null,
            switchMode: async ({ sessionId, mode }) => switchAicliModeForSession(sessionId, mode),
            executeCommand: async ({
              sessionId,
              command,
              model,
              reasoning,
              goal,
              task,
              replyId,
              taskId
            }) => {
              if (command === 'status') {
                return requestAicliStatusForSession(sessionId)
              }
              if (command === 'model') {
                return requestAicliModelForSession(sessionId, model, reasoning)
              }
              if (command === 'goal') {
                return requestAicliGoalForSession(sessionId, goal)
              }
              if (command === 'btw') {
                return requestAicliBtwForSession(sessionId, task ?? '', replyId, taskId)
              }
              if (command === 'interrupt') {
                return requestAicliInterruptForSession(sessionId)
              }
              if (command === 'compact') {
                return requestAicliCompactForSession(sessionId)
              }
              if (command === 'clear') {
                return requestAicliClearForSession(sessionId)
              }
              return {
                ok: false as const,
                error: 'unsupported AICLI control command'
              }
            },
            createDiffReport: ({ targetRepo, args: diffArgs }) =>
              createGitDiffReport({
                targetRepo,
                ...(diffArgs ? { args: diffArgs } : {}),
                outputDir: join(rootDir(), 'remote-im-diff-reports')
              }),
            args,
            ...(replyId ? { replyId } : {}),
            ...(taskId ? { taskId } : {})
          })
        },
        store: {
          create: (input) => createRemoteImMessage(input),
          updateStatus: (id, patch) =>
            updateRemoteImMessageStatus(id, {
              status: patch.status ?? 'received',
              sessionId: patch.sessionId,
              error: patch.error,
              sentToAicliAt: patch.sentToAicliAt,
              sentToImAt: patch.sentToImAt
            })
        }
      })
      const result = await router.handleIncomingText(message)
      broadcastMessagesChanged(message.projectId)
      return result
      })
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-audio',
    async (
      _event,
      {
        message,
        runtimeIdentity
      }: { message: RemoteImIncomingAudioMessage; runtimeIdentity: RemoteImRuntimeIdentity }
    ) =>
      withRemoteImIncomingOperation(async (securityGeneration) => {
      if (!isCurrentRemoteImRuntime(message.projectId, runtimeIdentity)) {
        return { ok: false as const, error: 'Remote IM runtime is stale' }
      }
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveRemoteImSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        ...createOutputRoutingDeps(config, securityGeneration),
        sendUser: sendUserMessageToSession,
        sendImText,
        store: {
          create: (input) => createRemoteImMessage(input),
          updateStatus: (id, patch) =>
            updateRemoteImMessageStatus(id, {
              status: patch.status ?? 'received',
              sessionId: patch.sessionId,
              error: patch.error,
              sentToAicliAt: patch.sentToAicliAt,
              sentToImAt: patch.sentToImAt
            })
        }
      })
      const result = await router.handleIncomingAudio(message)
      broadcastMessagesChanged(message.projectId)
      return result
      })
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-image',
    async (
      _event,
      {
        message,
        runtimeIdentity
      }: { message: RemoteImIncomingImageMessage; runtimeIdentity: RemoteImRuntimeIdentity }
    ) =>
      withRemoteImIncomingOperation(async (securityGeneration) => {
      if (!isCurrentRemoteImRuntime(message.projectId, runtimeIdentity)) {
        return { ok: false as const, error: 'Remote IM runtime is stale' }
      }
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveRemoteImSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        ...createOutputRoutingDeps(config, securityGeneration),
        sendUser: sendUserMessageToSession,
        sendImText,
        cacheImage: async (incoming) => {
          try {
            const cached = await cacheRemoteImImage({
              rootDir: rootDir(),
              projectId: incoming.projectId,
              remoteUrl: incoming.imageUrl,
              remoteMessageId: incoming.remoteMessageId,
              fileName: incoming.fileName,
              mimeType: incoming.mimeType
            })
            return {
              ok: true as const,
              attachment: imageAttachmentFromIncoming(incoming, {
                localPath: cached.localPath,
                fileName: cached.fileName,
                mimeType: cached.mimeType,
                sizeBytes: cached.sizeBytes
              })
            }
          } catch (err) {
            return {
              ok: false as const,
              error: err instanceof Error ? err.message : String(err),
              attachment: imageAttachmentFromIncoming(incoming)
            }
          }
        },
        store: {
          create: (input) => createRemoteImMessage(input),
          updateStatus: (id, patch) =>
            updateRemoteImMessageStatus(id, {
              status: patch.status ?? 'received',
              sessionId: patch.sessionId,
              error: patch.error,
              sentToAicliAt: patch.sentToAicliAt,
              sentToImAt: patch.sentToImAt
            })
        }
      })
      const result = await router.handleIncomingImage(message)
      broadcastMessagesChanged(message.projectId)
      return result
      })
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-file',
    async (
      _event,
      {
        message,
        runtimeIdentity
      }: { message: RemoteImIncomingFileMessage; runtimeIdentity: RemoteImRuntimeIdentity }
    ) =>
      withRemoteImIncomingOperation(async (securityGeneration) => {
      if (!isCurrentRemoteImRuntime(message.projectId, runtimeIdentity)) {
        return { ok: false as const, error: 'Remote IM runtime is stale' }
      }
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveRemoteImSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        ...createOutputRoutingDeps(config, securityGeneration),
        sendUser: sendUserMessageToSession,
        sendImText,
        cacheFile: async (incoming) => {
          const fallbackAttachment = fileAttachmentFromIncoming(incoming)
          try {
            const cached = await cacheRemoteImFile({
              rootDir: rootDir(),
              projectId: incoming.projectId,
              remoteUrl: incoming.fileUrl,
              remoteMessageId: incoming.remoteMessageId,
              fileName: incoming.fileName,
              mimeType: incoming.mimeType,
              maxBytes: MAX_REMOTE_IM_FILE_BYTES
            })
            const attachment: RemoteImFileAttachment = {
              ...fallbackAttachment,
              localPath: cached.localPath,
              fileName: cached.fileName,
              mimeType: cached.mimeType,
              sizeBytes: cached.sizeBytes
            }
            return { ok: true as const, attachment }
          } catch (err) {
            return {
              ok: false as const,
              error: err instanceof Error ? err.message : String(err),
              attachment: fallbackAttachment
            }
          }
        },
        store: {
          create: (input) => createRemoteImMessage(input),
          updateStatus: (id, patch) =>
            updateRemoteImMessageStatus(id, {
              status: patch.status ?? 'received',
              sessionId: patch.sessionId,
              error: patch.error,
              sentToAicliAt: patch.sentToAicliAt,
              sentToImAt: patch.sentToImAt
            })
        }
      })
      const result = await router.handleIncomingFile(message)
      broadcastMessagesChanged(message.projectId)
      return result
      })
  )

  // 按会话向上翻页：取严格早于给定锚点的更早消息（键集分页）。
  ipcMain.handle(
    'remote-im:list-peer-messages-before',
    (
      _event,
      {
        projectId,
        peerUserId,
        beforeCreatedAt,
        beforeId,
        limit
      }: {
        projectId: string
        peerUserId: string
        beforeCreatedAt: number
        beforeId: number
        limit?: number
      }
    ) => {
      if (!projectId || !peerUserId?.trim()) return []
      return listRemoteImPeerMessagesBefore(
        projectId,
        peerUserId,
        beforeCreatedAt,
        beforeId,
        limit ?? 200
      )
    }
  )

  // SDK 漫游补拉：登录后补充离线期间的历史消息，只入库展示、不路由 AICLI。
  ipcMain.handle(
    'remote-im:backfill-roamed-text',
    async (
      _event,
      {
        projectId,
        messages,
        runtimeIdentity
      }: {
        projectId: string
        messages: RemoteImRoamedTextMessage[]
        runtimeIdentity: RemoteImRuntimeIdentity
      }
    ) => withRemoteImAccountBoundOperation(async () => {
      if (!isCurrentRemoteImRuntime(projectId, runtimeIdentity)) {
        return { ok: false as const, error: 'Remote IM runtime is stale' }
      }
      if (!projectId || !Array.isArray(messages) || messages.length === 0) {
        return { ok: true as const, inserted: 0 }
      }
      const config = await getRemoteImConfig(projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => null,
        sendUser: async () => ({ ok: false, error: 'backfill does not route' }),
        sendImText: async () => ({
          ok: false,
          error: 'backfill does not send'
        }),
        store: {
          create: (input) => createRemoteImMessage(input),
          updateStatus: (id, patch) =>
            updateRemoteImMessageStatus(id, {
              status: patch.status ?? 'received',
              sessionId: patch.sessionId,
              error: patch.error,
              sentToAicliAt: patch.sentToAicliAt,
              sentToImAt: patch.sentToImAt
            }),
          findByRemoteMessageId: (provider, remoteMessageId) =>
            findRemoteImMessageByRemoteId(provider, remoteMessageId)
        }
      })
      const result = await router.backfillRoamedText(projectId, messages)
      if (result.inserted > 0) broadcastMessagesChanged(projectId)
      return result
    })
  )

  ipcMain.handle(
    'remote-im:send-local-message',
    async (_event, { projectId, text }: { projectId: string; text: string }) => {
      const session = getActiveSessionForProject(projectId)
      if (!session) return { ok: false as const, error: 'No running AICLI session' }
      const result = await sendUserMessageToSession(session.sessionId, text)
      if (result.ok) {
        createRemoteImMessage({
          projectId,
          sessionId: session.sessionId,
          provider: 'tencent-im',
          role: 'system',
          direction: 'internal',
          content: text,
          status: 'sent-to-aicli',
          createdAt: Date.now(),
          sentToAicliAt: Date.now()
        })
        broadcastMessagesChanged(projectId)
      }
      return result
    }
  )

  ipcMain.handle(
    'remote-im:send-peer-message',
    async (
      _event,
      { projectId, text, toUserId }: { projectId: string; text: string; toUserId?: string | null }
    ) => {
      return await withRemoteImAccountBoundOperation(() =>
        sendRemoteImPeerMessage(projectId, text, toUserId, 'human')
      )
    }
  )

  ipcMain.handle(
    'remote-im:send-peer-image',
    async (
      _event,
      input: {
        projectId: string
        fileToken: string
        toUserId?: string | null
        localPath?: string | null
        fileName?: string | null
        mimeType?: string | null
        sizeBytes?: number | null
      }
    ) => {
      return await withRemoteImAccountBoundOperation(() =>
        sendRemoteImPeerImage({ ...input, origin: 'human' })
      )
    }
  )

  ipcMain.handle(
    'remote-im:send-peer-file',
    async (
      _event,
      input: {
        projectId: string
        localPath: string
        toUserId?: string | null
      }
    ) => {
      return await withRemoteImAccountBoundOperation(() =>
        sendRemoteImPeerLocalFile(input.projectId, input.localPath, input.toUserId, 'human')
      )
    }
  )

  ipcMain.handle(
    'remote-im:read-file-preview',
    async (
      _event,
      input: {
        localPath?: string | null
        mimeType?: string | null
      }
    ) => {
      return await readRemoteImFilePreview(input)
    }
  )
}
