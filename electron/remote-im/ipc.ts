import { BrowserWindow, ipcMain } from 'electron'
import { promises as fs } from 'fs'
import { join } from 'path'
import {
  addSessionDataListener,
  addSessionExitListener,
  getActiveSessionForProject,
  getSessionRuntimeInfo,
  requestAicliStatusForSession,
  sendUserMessageToSession,
  switchAicliModeForSession
} from '../cc/ptyManager.js'
import { projectDir, rootDir } from '../store/paths.js'
import { readProjectMetaFile, writeProjectMetaFile, type ProjectMeta } from '../store/projectMeta.js'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  toRemoteImProjectConfig,
  validateRemoteImConfig
} from './config.js'
import {
  hasRemoteImAccountConnectionChanged,
  mergeRemoteImAccountIntoConfig,
  normalizeRemoteImAccountConfig,
  readRemoteImAccountConfig,
  writeRemoteImAccountConfig
} from './account.js'
import {
  clearRemoteImMessages,
  clearRemoteImPeerMessages,
  createRemoteImMessage,
  failRemoteImMessageIfStreaming,
  listRemoteImMessages,
  updateRemoteImMessageStatus
} from './messageStore.js'
import {
  completeRemoteImOutputSession,
  flushRemoteImOutputSession,
  type RemoteImOutputCompletionInfo,
  type RemoteImOutputSessionState
} from './outputForwarding.js'
import { getRemoteImAicliOutputSourceKind } from './aicliSourceKind.js'
import {
  createPeerOutgoingImageMessageInput,
  createPeerOutgoingMessageInput,
  resolvePeerUserId
} from './peerMessage.js'
import { getRemoteImAccountProfileId, getRemoteImProfileId } from './profile.js'
import { createRemoteImRouter } from './router.js'
import { appendRemoteImRuntimeLog } from './runtimeLog.js'
import { readLatestClaudeRemoteImReply } from './claudeTranscript.js'
import { startRemoteImCliServer } from './imcliServer.js'
import { addAicliStructuredOutputListener } from '../aicli/structuredOutputBridge.js'
import { executeRemoteImControlCommand } from './controlBridge.js'
import {
  createRemoteImAccountChangedStatuses,
  getRemoteImSendConnectionError
} from './status.js'
import { transcribeRemoteImAudioWithLocalWhisper } from './localWhisper.js'
import { cacheRemoteImImage } from './imageCache.js'
import {
  loadRemoteImLocalImageForSend,
  type RemoteImLocalImagePayload
} from './localImageFile.js'
import type {
  RemoteImAccountConfig,
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingTextMessage,
  RemoteImImageAttachment,
  RemoteImRuntimeLogEntryInput,
  RemoteImLoginState,
  RemoteImStatus
} from './types.js'

const REMOTE_IM_META_KEY = 'remote_im_config'
const DEFAULT_REMOTE_IM_PROFILE_ID = 'default'
const OUTGOING_DELIVERY_ACK_TIMEOUT_MS = 17_000
const MAX_REMOTE_IM_IMAGE_BYTES = 20 * 1024 * 1024

const statuses = new Map<string, RemoteImStatus>()
const outputSessions = new Map<string, RemoteImOutputSessionState>()
let activeRemoteImAccountProfileId: string | null = getRemoteImProfileId()

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

function broadcastMessagesChanged(projectId: string | null): void {
  broadcast('remote-im:messages-changed', { projectId })
}

function broadcastOutgoingText(
  projectId: string,
  toUserId: string,
  text: string,
  messageId?: number
): void {
  broadcast('remote-im:outgoing-text', { projectId, toUserId, text, messageId })
}

function broadcastOutgoingImage(
  projectId: string,
  toUserId: string,
  fileToken: string,
  messageId?: number
): void {
  broadcast('remote-im:outgoing-image', { projectId, toUserId, fileToken, messageId })
}

function broadcastOutgoingImagePayload(
  projectId: string,
  toUserId: string,
  image: RemoteImLocalImagePayload,
  messageId?: number
): void {
  broadcast('remote-im:outgoing-image', {
    projectId,
    toUserId,
    messageId,
    fileToken: null,
    fileName: image.fileName,
    mimeType: image.mimeType,
    fileBytes: image.fileBytes
  })
}

function scheduleOutgoingDeliveryAckTimeout(projectId: string, messageId: number): void {
  setTimeout(() => {
    const updated = failRemoteImMessageIfStreaming(
      messageId,
      'Remote IM sender window did not confirm delivery'
    )
    if (updated?.status === 'failed') {
      broadcastMessagesChanged(projectId)
    }
  }, OUTGOING_DELIVERY_ACK_TIMEOUT_MS)
}

async function readProjectMeta(projectId: string): Promise<{ meta: ProjectMeta; repaired: boolean }> {
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
  | { ok: false; error: string; details?: Array<{ path: string; message: string }> }
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
  broadcastStatus({
    projectId,
    state: 'disconnected',
    detail: null,
    updatedAt: Date.now()
  })
  return { ok: true, value: mergedConfig, ...(repaired ? { repaired: true as const } : {}) }
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
  broadcastOutgoingText(projectId, toUserId, text, options.messageId)
  if (options.messageId) {
    scheduleOutgoingDeliveryAckTimeout(projectId, options.messageId)
  }
  return Promise.resolve({ ok: true })
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

function removeRemoteImAccountContact(
  account: RemoteImAccountConfig,
  rawUserId: string
): RemoteImAccountConfig {
  const userId = rawUserId.trim()
  if (!userId) return account
  const removeUserId = (userIds: string[]) => userIds.filter((item) => item.trim() !== userId)
  return normalizeRemoteImAccountConfig({
    ...account,
    friendUserIds: removeUserId(account.friendUserIds),
    masterUserIds: removeUserId(account.masterUserIds),
    slaveUserIds: removeUserId(account.slaveUserIds),
    allowedUserIds: removeUserId(account.allowedUserIds)
  })
}

async function deleteRemoteImContact(
  projectId: string,
  rawUserId: string
): Promise<
  | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
  | { ok: false; error: string }
> {
  const userId = rawUserId.trim()
  if (!userId) return { ok: false, error: '请填写账号 ID' }

  const previousProfileId = getCurrentRemoteImAccountProfileId()
  const previousAccount = previousProfileId
    ? await readRemoteImAccountConfig(remoteImAccountDir(previousProfileId))
    : normalizeRemoteImAccountConfig(null)
  const nextAccount = removeRemoteImAccountContact(previousAccount, userId)
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

async function sendRemoteImPeerMessage(
  projectId: string,
  text: string,
  toUserId?: string | null
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

  const message = createRemoteImMessage(
    createPeerOutgoingMessageInput({
      projectId,
      config,
      toUserId: peerUserId,
      text: cleanText,
      now: Date.now()
    })
  )
  broadcastOutgoingText(projectId, peerUserId, cleanText, message.id)
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

  const attachment: RemoteImImageAttachment = {
    type: 'image',
    localPath: input.localPath?.trim() || null,
    remoteUrl: null,
    thumbnailUrl: null,
    width: null,
    height: null,
    sizeBytes: input.sizeBytes ?? null,
    fileName: input.fileName?.trim() || null,
    mimeType: input.mimeType?.trim() || null,
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
  broadcastOutgoingImage(input.projectId, peerUserId, fileToken, message.id)
  scheduleOutgoingDeliveryAckTimeout(input.projectId, message.id)
  broadcastMessagesChanged(input.projectId)
  return { ok: true, toUserId: peerUserId }
}

async function sendRemoteImPeerLocalImage(
  projectId: string,
  localPath: string,
  toUserId?: string | null
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

  let payload: RemoteImLocalImagePayload
  try {
    payload = await loadRemoteImLocalImageForSend(cleanPath, {
      maxBytes: MAX_REMOTE_IM_IMAGE_BYTES
    })
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  }

  const message = createRemoteImMessage(
    createPeerOutgoingImageMessageInput({
      projectId,
      config,
      toUserId: peerUserId,
      attachment: payload.attachment,
      now: Date.now()
    })
  )
  broadcastOutgoingImagePayload(projectId, peerUserId, payload, message.id)
  scheduleOutgoingDeliveryAckTimeout(projectId, message.id)
  broadcastMessagesChanged(projectId)
  return { ok: true, toUserId: peerUserId }
}

function readRemoteImTranscriptReply(source: NonNullable<RemoteImOutputSessionState['transcript']>): string | null {
  if (source.kind === 'claude') {
    return readLatestClaudeRemoteImReply({
      cwd: source.cwd,
      sinceMs: source.sinceMs,
      replyId: source.replyId
    })
  }
  return null
}

function startOutputForwarding(
  sessionId: string,
  projectId: string,
  toUserId: string,
  config: RemoteImConfig,
  replyId?: string
): void {
  const current = outputSessions.get(sessionId)
  if (current?.timer) clearTimeout(current.timer)
  const runtime = getSessionRuntimeInfo(sessionId)
  const sourceKind = runtime
    ? getRemoteImAicliOutputSourceKind(runtime.command)
    : 'unknown'
  outputSessions.set(sessionId, {
    projectId,
    toUserId,
    config,
    replyId,
    sourceKind,
    buffer: sourceKind === 'codex' || sourceKind === 'opencode' ? '' : current?.buffer ?? '',
    timer: null,
    structuredOutput: sourceKind === 'codex' || sourceKind === 'opencode',
    transcript:
      sourceKind === 'claude' && runtime
        ? {
            kind: sourceKind,
            cwd: runtime.targetRepo,
            sinceMs: Date.now(),
            replyId
          }
        : undefined
  })
}

function flushOutputSession(sessionId: string): void {
  const state = outputSessions.get(sessionId)
  if (!state) return
  flushRemoteImOutputSession(sessionId, state, {
    createMessage: (input) => {
      createRemoteImMessage(input)
    },
    sendText: broadcastOutgoingText,
    messagesChanged: broadcastMessagesChanged,
    readTranscriptReply: readRemoteImTranscriptReply
  })
}

function completeOutputSession(
  sessionId: string,
  info: RemoteImOutputCompletionInfo = {}
): void {
  const state = outputSessions.get(sessionId)
  if (!state) return
  completeRemoteImOutputSession(
    sessionId,
    state,
    {
      createMessage: (input) => {
        createRemoteImMessage(input)
      },
      sendText: broadcastOutgoingText,
      messagesChanged: broadcastMessagesChanged,
      readTranscriptReply: readRemoteImTranscriptReply
    },
    info
  )
  outputSessions.delete(sessionId)
}

function scheduleOutputFlush(sessionId: string): void {
  const state = outputSessions.get(sessionId)
  if (!state || state.timer) return
  state.timer = setTimeout(() => flushOutputSession(sessionId), state.config.outputFlushIntervalMs)
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
  addAicliStructuredOutputListener(({ sessionId, text }) => {
    const state = outputSessions.get(sessionId)
    if (!state) return
    if (!state.structuredOutput) return
    // 结构化输出的每个 text 是一段完整 assistant 正文，段间必须补换行：
    // marker 提取按行匹配，若旁白与最终答复落在同一个防抖窗口被无分隔拼接，
    // open 标签会失去独立行导致提取为空，且 flush 会把整个 buffer（含回复）清掉。
    state.buffer += (state.buffer !== '' && !state.buffer.endsWith('\n') ? '\n' : '') + text
    scheduleOutputFlush(sessionId)
  })
  addSessionExitListener(({ sessionId, exitCode, signal }) => {
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
    sendPeerMessage: sendRemoteImPeerMessage,
    sendPeerImage: sendRemoteImPeerLocalImage
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
  const normalizedAccount = normalizeRemoteImAccountConfig(account)
  const previousProfileId = getCurrentRemoteImAccountProfileId()
  const previousAccount = previousProfileId
    ? await readRemoteImAccountConfig(remoteImAccountDir(previousProfileId))
    : normalizeRemoteImAccountConfig(null)
  const profileId =
    getRemoteImAccountProfileId(normalizedAccount.desktopUserId) ??
    getRemoteImProfileId() ??
    DEFAULT_REMOTE_IM_PROFILE_ID
  activeRemoteImAccountProfileId = profileId
  const value = await writeRemoteImAccountConfig(remoteImAccountDir(profileId), normalizedAccount)
  if (hasRemoteImAccountConnectionChanged(previousAccount, value)) {
    resetRemoteImStatusesAfterAccountChange()
  }
  return { profileId, account: value }
}

export function registerRemoteImIpc(options: RegisterRemoteImIpcOptions = {}): void {
  // ensureSessionListeners 只挂 PTY 输出/退出监听，登录前也安全；imcli 桥接服务
  // （ensureRemoteImCliServer）依赖账号作用域 rootDir，移到 activateRemoteImDataLayer。
  ensureSessionListeners()

  ipcMain.handle(
    'remote-im:bind-account',
    async (
      _event,
      { account }: { account: RemoteImAccountConfig }
    ) => {
      try {
        const normalized = normalizeRemoteImAccountConfig(account)
        const userId = normalized.desktopUserId?.trim()
        if (!userId) return { ok: false as const, error: '请填写 IM 账号（desktopUserId）' }
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
                : activated.error ?? '账号数据层初始化失败'
          }
        }
        // 2) 写账号配置（此时 rootDir 已按账号作用域就绪）
        await bindRemoteImAccountConfig(normalized)
        // 3) 读回登录态供渲染层解锁登录门
        return { ok: true as const, value: await getRemoteImLoginState() }
      } catch (err) {
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
      }
    }
  )

  ipcMain.handle('remote-im:get-config', async (_event, { projectId }: { projectId: string }) => {
    try {
      return { ok: true as const, value: await getRemoteImConfig(projectId) }
    } catch (err) {
      return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
    }
  })

  ipcMain.handle('remote-im:get-login-state', async () => {
    try {
      return { ok: true as const, value: await getRemoteImLoginState() }
    } catch (err) {
      return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
    }
  })

  ipcMain.handle(
    'remote-im:get-account-by-user-id',
    async (_event, { userId }: { userId: string }) => {
      try {
        return { ok: true as const, value: await getRemoteImAccountByUserId(userId) }
      } catch (err) {
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
      }
    }
  )

  ipcMain.handle(
    'remote-im:set-account',
    async (_event, { account }: { account: RemoteImAccountConfig }) => {
      try {
        return { ok: true as const, value: await bindRemoteImAccountConfig(account) }
      } catch (err) {
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
      }
    }
  )

  ipcMain.handle(
    'remote-im:set-config',
    async (_event, { projectId, config }: { projectId: string; config: unknown }) => {
      try {
        return await setRemoteImConfig(projectId, config)
      } catch (err) {
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
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

  ipcMain.handle('remote-im:clear-messages', (_event, { projectId }: { projectId: string }) => {
    clearRemoteImMessages(projectId)
    broadcastMessagesChanged(projectId)
    return { ok: true as const }
  })

  ipcMain.handle(
    'remote-im:delete-contact',
    async (_event, { projectId, userId }: { projectId: string; userId: string }) => {
      try {
        return await deleteRemoteImContact(projectId, userId)
      } catch (err) {
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
      }
    }
  )

  ipcMain.handle(
    'remote-im:update-sdk-status',
    (_event, status: Pick<RemoteImStatus, 'projectId' | 'state' | 'detail'>) => {
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
    (_event, { projectId, messageId }: { projectId: string; messageId: number }) => {
      updateRemoteImMessageStatus(messageId, {
        status: 'sent-to-im',
        error: null,
        sentToImAt: Date.now()
      })
      broadcastMessagesChanged(projectId)
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'remote-im:mark-outgoing-message-failed',
    (
      _event,
      { projectId, messageId, error }: { projectId: string; messageId: number; error: string }
    ) => {
      updateRemoteImMessageStatus(messageId, {
        status: 'failed',
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
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
      }
    }
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-text',
    async (_event, message: RemoteImIncomingTextMessage) => {
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        sendUser: sendUserMessageToSession,
        sendImText,
        handleControlCommand: async ({ command }) => {
          const runtime = session ? getSessionRuntimeInfo(session.sessionId) : null
          const sourceKind = runtime
            ? getRemoteImAicliOutputSourceKind(runtime.command)
            : 'unknown'
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
            switchMode: async ({ sessionId, mode }) =>
              switchAicliModeForSession(sessionId, mode),
            executeCommand: async ({ sessionId, command }) => {
              if (command !== 'status') {
                return { ok: false as const, error: 'unsupported AICLI control command' }
              }
              return requestAicliStatusForSession(sessionId)
            }
          })
        },
        transcribeAudio: transcribeRemoteImAudioWithLocalWhisper,
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
      if (result.ok && result.aicliSessionId) {
        startOutputForwarding(result.aicliSessionId, message.projectId, message.fromUserId, config, result.replyId)
      }
      broadcastMessagesChanged(message.projectId)
      return result
    }
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-audio',
    async (_event, message: RemoteImIncomingAudioMessage) => {
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        sendUser: sendUserMessageToSession,
        sendImText,
        transcribeAudio: transcribeRemoteImAudioWithLocalWhisper,
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
      if (result.ok && result.aicliSessionId) {
        startOutputForwarding(result.aicliSessionId, message.projectId, message.fromUserId, config, result.replyId)
      }
      broadcastMessagesChanged(message.projectId)
      return result
    }
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-image',
    async (_event, message: RemoteImIncomingImageMessage) => {
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        sendUser: sendUserMessageToSession,
        sendImText,
        transcribeAudio: transcribeRemoteImAudioWithLocalWhisper,
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
      if (result.ok && result.aicliSessionId) {
        startOutputForwarding(result.aicliSessionId, message.projectId, message.fromUserId, config, result.replyId)
      }
      broadcastMessagesChanged(message.projectId)
      return result
    }
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
      return await sendRemoteImPeerMessage(projectId, text, toUserId)
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
      return await sendRemoteImPeerImage(input)
    }
  )
}
