import type {
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingFileMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingTextMessage,
  RemoteImRuntimeLogEntryInput
} from '../../electron/preload.js'

export interface TencentImTextMessage {
  remoteMessageId: string | null
  fromUserId: string
  toUserId: string | null
  text: string
  createdAt?: number
}

export interface TencentImAudioMessage {
  remoteMessageId: string | null
  fromUserId: string
  toUserId: string | null
  audioUrl: string
  durationSeconds: number | null
  sizeBytes: number | null
  uuid: string | null
  createdAt?: number
}

export interface TencentImImageMessage {
  remoteMessageId: string | null
  fromUserId: string
  toUserId: string | null
  imageUrl: string
  thumbnailUrl: string | null
  width: number | null
  height: number | null
  sizeBytes: number | null
  uuid: string | null
  fileName: string | null
  mimeType: string | null
  createdAt?: number
}

export interface TencentImFileMessage {
  remoteMessageId: string | null
  fromUserId: string
  toUserId: string | null
  fileUrl: string
  sizeBytes: number | null
  uuid: string | null
  fileName: string | null
  mimeType: string | null
  createdAt?: number
}

export function extractUserSig(payload: unknown): string {
  if (payload && typeof payload === 'object') {
    const value = payload as { userSig?: unknown }
    if (typeof value.userSig === 'string' && value.userSig.trim()) {
      return value.userSig.trim()
    }
  }
  throw new Error('凭证接口响应缺少有效凭证')
}

export interface GenerateTencentUserSigInput {
  sdkAppId: number
  userId: string
  secretKey: string
  expireSeconds?: number
  nowSeconds?: number
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = ''
  const chunkSize = 0x8000
  for (let index = 0; index < bytes.length; index += chunkSize) {
    const chunk = bytes.slice(index, index + chunkSize)
    binary += String.fromCharCode(...chunk)
  }
  return btoa(binary)
}

function toTencentBase64Url(base64: string): string {
  return base64.replace(/\+/g, '*').replace(/\//g, '-').replace(/=/g, '_')
}

async function hmacSha256Base64(secretKey: string, content: string): Promise<string> {
  const encoder = new TextEncoder()
  const key = await globalThis.crypto.subtle.importKey(
    'raw',
    encoder.encode(secretKey),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign']
  )
  const signature = await globalThis.crypto.subtle.sign('HMAC', key, encoder.encode(content))
  return bytesToBase64(new Uint8Array(signature))
}

async function deflateUtf8(input: string): Promise<Uint8Array> {
  if (typeof CompressionStream === 'undefined') {
    throw new Error('当前运行环境不支持生成本地登录凭证')
  }
  const stream = new Blob([input]).stream().pipeThrough(new CompressionStream('deflate'))
  return new Uint8Array(await new Response(stream).arrayBuffer())
}

export async function generateTencentUserSig(input: GenerateTencentUserSigInput): Promise<string> {
  const userId = input.userId.trim()
  const secretKey = input.secretKey.trim()
  if (!Number.isInteger(input.sdkAppId) || input.sdkAppId <= 0) {
    throw new Error('IM 应用配置无效')
  }
  if (!userId) throw new Error('请填写登录账号')
  if (!secretKey) throw new Error('内置连接凭证无效')

  const expireSeconds = input.expireSeconds ?? 604800
  const nowSeconds = input.nowSeconds ?? Math.floor(Date.now() / 1000)
  const contentToSign =
    `TLS.identifier:${userId}\n` +
    `TLS.sdkappid:${input.sdkAppId}\n` +
    `TLS.time:${nowSeconds}\n` +
    `TLS.expire:${expireSeconds}\n`
  const sig = await hmacSha256Base64(secretKey, contentToSign)
  const payload = {
    'TLS.ver': '2.0',
    'TLS.identifier': userId,
    'TLS.sdkappid': input.sdkAppId,
    'TLS.expire': expireSeconds,
    'TLS.time': nowSeconds,
    'TLS.sig': sig
  }
  return toTencentBase64Url(bytesToBase64(await deflateUtf8(JSON.stringify(payload))))
}

function getTextPayload(message: Record<string, unknown>): string | null {
  const payload = message.payload
  if (!payload || typeof payload !== 'object') return null
  const text = (payload as { text?: unknown }).text
  return typeof text === 'string' && text.trim() ? text : null
}

function getStringField(source: Record<string, unknown>, keys: string[]): string | null {
  for (const key of keys) {
    const value = source[key]
    if (typeof value === 'string' && value.trim()) return value.trim()
  }
  return null
}

function getTencentImFriendListPayload(payload: unknown): unknown[] {
  if (Array.isArray(payload)) return payload
  if (!payload || typeof payload !== 'object') return []
  const raw = payload as Record<string, unknown>
  for (const key of ['data', 'friendList', 'friends', 'list', 'items']) {
    const nested = getTencentImFriendListPayload(raw[key])
    if (nested.length > 0) return nested
  }
  return []
}

function getTencentImFriendUserId(friend: unknown): string | null {
  if (typeof friend === 'string') return friend.trim() || null
  if (!friend || typeof friend !== 'object') return null
  const raw = friend as Record<string, unknown>
  const direct = getStringField(raw, [
    'userID',
    'userId',
    'userIDList',
    'identifier',
    'friendUserID',
    'friendUserId'
  ])
  if (direct) return direct
  for (const key of ['profile', 'friendProfile', 'userProfile', 'friendInfo', 'userInfo']) {
    const nested = raw[key]
    if (nested && typeof nested === 'object') {
      const userId = getTencentImFriendUserId(nested)
      if (userId) return userId
    }
  }
  return null
}

export function extractTencentImFriendUserIds(payload: unknown): string[] {
  return Array.from(
    new Set(
      getTencentImFriendListPayload(payload)
        .map((friend) => getTencentImFriendUserId(friend))
        .filter((userId): userId is string => Boolean(userId))
    )
  )
}

function getNumberField(source: Record<string, unknown>, keys: string[]): number | null {
  for (const key of keys) {
    const value = source[key]
    if (typeof value === 'number' && Number.isFinite(value)) return value
    if (typeof value === 'string') {
      const parsed = Number(value.trim())
      if (Number.isFinite(parsed)) return parsed
    }
  }
  return null
}

function isTencentAudioMessage(message: Record<string, unknown>): boolean {
  const type = typeof message.type === 'string' ? message.type : ''
  return type === 'TIMSoundElem' || type === 'MSG_AUDIO'
}

function isTencentImageMessage(message: Record<string, unknown>): boolean {
  const type = typeof message.type === 'string' ? message.type : ''
  return type === 'TIMImageElem' || type === 'MSG_IMAGE'
}

function isTencentFileMessage(message: Record<string, unknown>): boolean {
  const type = typeof message.type === 'string' ? message.type : ''
  return type === 'TIMFileElem' || type === 'MSG_FILE'
}

function mimeTypeFromFileName(fileName: string | null): string | null {
  const ext = fileName?.split('.').pop()?.trim().toLowerCase()
  switch (ext) {
    case 'md':
    case 'markdown':
      return 'text/markdown'
    case 'html':
    case 'htm':
      return 'text/html'
    default:
      return null
  }
}

function getAudioPayload(
  message: Record<string, unknown>
): Omit<TencentImAudioMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null {
  if (!isTencentAudioMessage(message)) return null
  const payload = message.payload
  if (!payload || typeof payload !== 'object') return null
  const audio = payload as Record<string, unknown>
  const audioUrl = getStringField(audio, ['url', 'URL', 'downloadUrl', 'downloadURL'])
  if (!audioUrl) return null
  return {
    audioUrl,
    durationSeconds: getNumberField(audio, ['duration', 'second', 'seconds', 'time']),
    sizeBytes: getNumberField(audio, ['size', 'dataSize', 'fileSize']),
    uuid: getStringField(audio, ['uuid', 'UUID', 'fileId', 'fileID'])
  }
}

function toRecordArray(value: unknown): Record<string, unknown>[] {
  if (!Array.isArray(value)) return []
  return value.filter((item): item is Record<string, unknown> => Boolean(item && typeof item === 'object'))
}

function getImageInfoArray(payload: Record<string, unknown>): Record<string, unknown>[] {
  for (const key of ['imageInfoArray', 'imageList', 'imageArray', 'images']) {
    const images = toRecordArray(payload[key])
    if (images.length > 0) return images
  }
  return []
}

function getImageInfoUrl(info: Record<string, unknown>): string | null {
  return getStringField(info, ['url', 'URL', 'imageUrl', 'imageURL', 'downloadUrl', 'downloadURL'])
}

function getImageInfoScore(info: Record<string, unknown>): number {
  const size = getNumberField(info, ['size', 'dataSize', 'fileSize'])
  if (size !== null) return size
  const width = getNumberField(info, ['width', 'imageWidth'])
  const height = getNumberField(info, ['height', 'imageHeight'])
  return (width ?? 0) * (height ?? 0)
}

function getImagePayload(
  message: Record<string, unknown>
): Omit<TencentImImageMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null {
  if (!isTencentImageMessage(message)) return null
  const payload = message.payload
  if (!payload || typeof payload !== 'object') return null
  const image = payload as Record<string, unknown>
  const imageInfos = getImageInfoArray(image).filter((info) => Boolean(getImageInfoUrl(info)))

  const primaryInfo =
    imageInfos.length > 0
      ? imageInfos.reduce((current, next) =>
          getImageInfoScore(next) > getImageInfoScore(current) ? next : current
        )
      : image
  const imageUrl = getImageInfoUrl(primaryInfo)
  if (!imageUrl) return null

  const thumbnailInfo =
    imageInfos.length > 1
      ? imageInfos.reduce((current, next) =>
          getImageInfoScore(next) < getImageInfoScore(current) ? next : current
        )
      : null
  const rawMimeType = getStringField(image, ['mimeType', 'contentType'])
  return {
    imageUrl,
    thumbnailUrl: thumbnailInfo ? getImageInfoUrl(thumbnailInfo) : null,
    width: getNumberField(primaryInfo, ['width', 'imageWidth']),
    height: getNumberField(primaryInfo, ['height', 'imageHeight']),
    sizeBytes: getNumberField(primaryInfo, ['size', 'dataSize', 'fileSize']),
    uuid: getStringField(image, ['uuid', 'UUID', 'imageUUID', 'fileId', 'fileID']),
    fileName: getStringField(image, ['fileName', 'filename', 'name']),
    mimeType: rawMimeType?.startsWith('image/') ? rawMimeType : null
  }
}

function getFilePayload(
  message: Record<string, unknown>
): Omit<TencentImFileMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null {
  if (!isTencentFileMessage(message)) return null
  const payload = message.payload
  if (!payload || typeof payload !== 'object') return null
  const file = payload as Record<string, unknown>
  const fileUrl = getStringField(file, ['url', 'URL', 'downloadUrl', 'downloadURL'])
  if (!fileUrl) return null
  const fileName = getStringField(file, ['fileName', 'filename', 'name'])
  const mimeType =
    getStringField(file, ['mimeType', 'contentType']) ?? mimeTypeFromFileName(fileName)
  if (mimeType !== 'text/markdown' && mimeType !== 'text/html') return null
  return {
    fileUrl,
    sizeBytes: getNumberField(file, ['size', 'dataSize', 'fileSize']),
    uuid: getStringField(file, ['uuid', 'UUID', 'fileId', 'fileID']),
    fileName,
    mimeType
  }
}

export function extractTencentImTextMessages(event: unknown): TencentImTextMessage[] {
  const data = event && typeof event === 'object' ? (event as { data?: unknown }).data : null
  if (!Array.isArray(data)) return []
  return data.flatMap((item) => {
    if (!item || typeof item !== 'object') return []
    const message = item as Record<string, unknown>
    const text = getTextPayload(message)
    if (!text) return []
    const from = typeof message.from === 'string' ? message.from : ''
    if (!from) return []
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        text,
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

export function extractTencentImImageMessages(event: unknown): TencentImImageMessage[] {
  const data = event && typeof event === 'object' ? (event as { data?: unknown }).data : null
  if (!Array.isArray(data)) return []
  return data.flatMap((item) => {
    if (!item || typeof item !== 'object') return []
    const message = item as Record<string, unknown>
    const image = getImagePayload(message)
    if (!image) return []
    const from = typeof message.from === 'string' ? message.from : ''
    if (!from) return []
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        ...image,
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

export function extractTencentImFileMessages(event: unknown): TencentImFileMessage[] {
  const data = event && typeof event === 'object' ? (event as { data?: unknown }).data : null
  if (!Array.isArray(data)) return []
  return data.flatMap((item) => {
    if (!item || typeof item !== 'object') return []
    const message = item as Record<string, unknown>
    const file = getFilePayload(message)
    if (!file) return []
    const from = typeof message.from === 'string' ? message.from : ''
    if (!from) return []
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        ...file,
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

export function extractTencentImAudioMessages(event: unknown): TencentImAudioMessage[] {
  const data = event && typeof event === 'object' ? (event as { data?: unknown }).data : null
  if (!Array.isArray(data)) return []
  return data.flatMap((item) => {
    if (!item || typeof item !== 'object') return []
    const message = item as Record<string, unknown>
    const audio = getAudioPayload(message)
    if (!audio) return []
    const from = typeof message.from === 'string' ? message.from : ''
    if (!from) return []
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        ...audio,
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

async function requestUserSig(config: RemoteImConfig): Promise<string> {
  if (config.userSigMode === 'secret-key') {
    return generateTencentUserSig({
      sdkAppId: config.sdkAppId ?? 0,
      userId: config.desktopUserId,
      secretKey: config.userSigSecretKey
    })
  }
  const response = await fetch(config.userSigEndpoint, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      sdkAppId: config.sdkAppId,
      userId: config.desktopUserId
    })
  })
  if (!response.ok) {
    throw new Error(`凭证接口返回 HTTP ${response.status}`)
  }
  return extractUserSig(await response.json())
}

async function loadTencentImSdk(): Promise<any> {
  const mod: any = await import('@tencentcloud/lite-chat')
  return mod.default ?? mod.TencentCloudChat ?? mod
}

function waitForTencentImReady(chat: any, TencentCloudChat: any): Promise<void> {
  if (typeof chat.isReady === 'function' && chat.isReady()) return Promise.resolve()
  if (typeof chat.on !== 'function') return Promise.resolve()

  const eventName = TencentCloudChat.EVENT?.SDK_READY ?? 'sdkStateReady'
  return new Promise((resolve, reject) => {
    let settled = false
    const finish = (error?: Error): void => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      chat.off?.(eventName, onReady)
      if (error) reject(error)
      else resolve()
    }
    const onReady = (): void => finish()
    const timer = setTimeout(
      () => finish(new Error('IM SDK 就绪超时')),
      15_000
    )
    chat.on(eventName, onReady)
    if (typeof chat.isReady === 'function' && chat.isReady()) finish()
  })
}

function getTencentImApiFailure(action: string, result: unknown): string | null {
  if (!result || typeof result !== 'object') return null
  const response = result as { code?: unknown; message?: unknown }
  const code = Number(response.code ?? 0)
  if (!Number.isFinite(code) || code === 0) return null
  const message =
    typeof response.message === 'string' && response.message.trim()
      ? response.message.trim()
      : JSON.stringify(result)
  return `IM ${getTencentImApiActionLabel(action)}失败 (${code}): ${message}`
}

function getTencentImApiActionLabel(action: string): string {
  switch (action) {
    case 'login':
      return '登录'
    case 'send':
      return '发送'
    case 'friend-list':
      return '好友列表同步'
    default:
      return '操作'
  }
}

function summarizeTencentImApiResult(result: unknown): { code: number | null; message: string | null } {
  if (!result || typeof result !== 'object') return { code: null, message: null }
  const response = result as { code?: unknown; message?: unknown }
  const code = Number(response.code ?? 0)
  return {
    code: Number.isFinite(code) ? code : null,
    message: typeof response.message === 'string' ? response.message : null
  }
}

function summarizeTencentImMessage(message: unknown): Record<string, unknown> | null {
  if (!message || typeof message !== 'object') return null
  const raw = message as Record<string, unknown>
  return {
    ID: typeof raw.ID === 'string' ? raw.ID : null,
    conversationID: typeof raw.conversationID === 'string' ? raw.conversationID : null,
    to: typeof raw.to === 'string' ? raw.to : null,
    type: typeof raw.type === 'string' ? raw.type : null
  }
}

function getTencentImLoginUser(chat: any): string | null {
  if (typeof chat.getLoginUser !== 'function') return null
  const userId = chat.getLoginUser()
  return typeof userId === 'string' && userId.trim() ? userId.trim() : ''
}

async function loginTencentImClient(
  chat: any,
  TencentCloudChat: any,
  config: RemoteImConfig,
  emitRuntimeLog?: (event: string, patch?: Partial<RemoteImRuntimeLogEntryInput>) => void
): Promise<void> {
  try {
    emitRuntimeLog?.('login:user-sig:start', {
      detail: { mode: config.userSigMode }
    })
    const userSig = await requestUserSig(config)
    emitRuntimeLog?.('login:user-sig:ready', {
      detail: { mode: config.userSigMode }
    })
    emitRuntimeLog?.('login:start')
    const result = await chat.login({
      userID: config.desktopUserId,
      userSig
    })
    emitRuntimeLog?.('login:resolved', {
      detail: summarizeTencentImApiResult(result)
    })
    const failure = getTencentImApiFailure('login', result)
    if (failure) throw new Error(failure)
    emitRuntimeLog?.('ready:wait:start', {
      detail: {
        isReady: typeof chat.isReady === 'function' ? Boolean(chat.isReady()) : null
      }
    })
    await waitForTencentImReady(chat, TencentCloudChat)

    const loginUser = getTencentImLoginUser(chat)
    emitRuntimeLog?.('ready:wait:resolved', {
      detail: { loginUser }
    })
    if (loginUser !== null && loginUser !== config.desktopUserId) {
      throw new Error(
        loginUser
          ? `IM 已登录账号 ${loginUser}，预期 ${config.desktopUserId}`
          : 'IM 登录未建立有效会话'
      )
    }
  } catch (err) {
    emitRuntimeLog?.('login:failed', {
      detail: { error: err instanceof Error ? err.message : String(err) }
    })
    throw err
  }
}

export interface TencentImSendTextOptions {
  messageId?: number | null
}

export type TencentImSendImageOptions = TencentImSendTextOptions
export type TencentImSendFileOptions = TencentImSendTextOptions

export interface TencentImRuntime {
  disconnect(): Promise<void>
  listFriendUserIds?(): Promise<string[]>
  sendText(toUserId: string, text: string, options?: TencentImSendTextOptions): Promise<void>
  sendImage?(toUserId: string, file: File, options?: TencentImSendImageOptions): Promise<void>
  sendFile?(toUserId: string, file: File, options?: TencentImSendFileOptions): Promise<void>
}

export async function connectTencentImClient(input: {
  projectId: string
  config: RemoteImConfig
  onIncomingText: (message: RemoteImIncomingTextMessage) => void
  onIncomingAudio?: (message: RemoteImIncomingAudioMessage) => void
  onIncomingImage?: (message: RemoteImIncomingImageMessage) => void
  onIncomingFile?: (message: RemoteImIncomingFileMessage) => void
  onFriendListUpdated?: (userIds: string[]) => void
  onRuntimeLog?: (entry: RemoteImRuntimeLogEntryInput) => void
}): Promise<TencentImRuntime> {
  const emitRuntimeLog = (
    event: string,
    patch: Partial<RemoteImRuntimeLogEntryInput> = {}
  ): void => {
    input.onRuntimeLog?.({
      projectId: input.projectId,
      sdkAppId: input.config.sdkAppId,
      desktopUserId: input.config.desktopUserId,
      event,
      createdAt: Date.now(),
      ...patch
    })
  }

  emitRuntimeLog('connect:start')
  const TencentCloudChat = await loadTencentImSdk()
  const chat = TencentCloudChat.create({ SDKAppID: input.config.sdkAppId })
  emitRuntimeLog('sdk:create')
  chat.setLogLevel?.(1)
  let sdkReady = false
  let loggedInUserId: string | null = null
  // 被同账号在别处登录顶下线后置真：阻断 ensureLoggedIn 的自动重登，避免与另一端
  // 互踢死循环。只有重新 connect（disconnect 后再连）才复位。
  let kickedOut = false

  const onMessageReceived = (event: unknown): void => {
    const messages = extractTencentImTextMessages(event)
    const audioMessages = extractTencentImAudioMessages(event)
    const imageMessages = extractTencentImImageMessages(event)
    const fileMessages = extractTencentImFileMessages(event)
    emitRuntimeLog('message:received', {
      detail: {
        count: messages.length,
        audioCount: audioMessages.length,
        imageCount: imageMessages.length,
        fileCount: fileMessages.length
      }
    })
    for (const message of messages) {
      input.onIncomingText({
        projectId: input.projectId,
        remoteMessageId: message.remoteMessageId,
        fromUserId: message.fromUserId,
        toUserId: message.toUserId,
        text: message.text,
        createdAt: message.createdAt
      })
    }
    for (const message of audioMessages) {
      input.onIncomingAudio?.({
        projectId: input.projectId,
        remoteMessageId: message.remoteMessageId,
        fromUserId: message.fromUserId,
        toUserId: message.toUserId,
        audioUrl: message.audioUrl,
        durationSeconds: message.durationSeconds,
        sizeBytes: message.sizeBytes,
        uuid: message.uuid,
        createdAt: message.createdAt
      })
    }
    for (const message of imageMessages) {
      input.onIncomingImage?.({
        projectId: input.projectId,
        remoteMessageId: message.remoteMessageId,
        fromUserId: message.fromUserId,
        toUserId: message.toUserId,
        imageUrl: message.imageUrl,
        thumbnailUrl: message.thumbnailUrl,
        width: message.width,
        height: message.height,
        sizeBytes: message.sizeBytes,
        uuid: message.uuid,
        fileName: message.fileName,
        mimeType: message.mimeType,
        createdAt: message.createdAt
      })
    }
    for (const message of fileMessages) {
      input.onIncomingFile?.({
        projectId: input.projectId,
        remoteMessageId: message.remoteMessageId,
        fromUserId: message.fromUserId,
        toUserId: message.toUserId,
        fileUrl: message.fileUrl,
        sizeBytes: message.sizeBytes,
        uuid: message.uuid,
        fileName: message.fileName,
        mimeType: message.mimeType,
        createdAt: message.createdAt
      })
    }
  }

  const eventName = TencentCloudChat.EVENT?.MESSAGE_RECEIVED ?? 'messageReceived'
  const sdkReadyEventName = TencentCloudChat.EVENT?.SDK_READY ?? 'sdkStateReady'
  const sdkNotReadyEventName = TencentCloudChat.EVENT?.SDK_NOT_READY ?? 'sdkStateNotReady'
  const friendListUpdatedEventName =
    TencentCloudChat.EVENT?.FRIEND_LIST_UPDATED ?? 'onFriendListUpdated'
  const onSdkReady = (): void => {
    sdkReady = true
    emitRuntimeLog('sdk:ready')
  }
  const onSdkNotReady = (): void => {
    sdkReady = false
    emitRuntimeLog('sdk:not-ready')
  }
  const kickedOutEventName = TencentCloudChat.EVENT?.KICKED_OUT ?? 'kickedOut'
  const onKickedOut = (event: { data?: { type?: string } } | undefined): void => {
    const type = event?.data?.type
    kickedOut = true
    sdkReady = false
    loggedInUserId = null
    emitRuntimeLog('kicked-out', {
      detail: {
        type,
        multipleAccount:
          type === (TencentCloudChat.TYPES?.KICKED_OUT_MULTI_ACCOUNT ?? 'multipleAccount')
      }
    })
  }
  const onFriendListUpdated = (event: unknown): void => {
    const userIds = extractTencentImFriendUserIds(event)
    emitRuntimeLog('friend-list:updated', {
      detail: { count: userIds.length }
    })
    if (userIds.length > 0) input.onFriendListUpdated?.(userIds)
  }
  chat.on?.(eventName, onMessageReceived)
  chat.on?.(sdkReadyEventName, onSdkReady)
  chat.on?.(sdkNotReadyEventName, onSdkNotReady)
  chat.on?.(kickedOutEventName, onKickedOut)
  chat.on?.(friendListUpdatedEventName, onFriendListUpdated)
  await loginTencentImClient(chat, TencentCloudChat, input.config, emitRuntimeLog)
  sdkReady = true
  loggedInUserId = input.config.desktopUserId

  async function ensureLoggedIn(): Promise<void> {
    if (kickedOut) {
      emitRuntimeLog('login:skip-kicked-out')
      throw new Error('IM 账号已在别处登录，本端已被踢下线，已停止自动重登')
    }
    if (loggedInUserId === input.config.desktopUserId && sdkReady) return
    emitRuntimeLog('login:refresh-required', {
      detail: { sdkReady, loggedInUserId }
    })
    await loginTencentImClient(chat, TencentCloudChat, input.config, emitRuntimeLog)
    sdkReady = true
    loggedInUserId = input.config.desktopUserId
  }

  return {
    async disconnect() {
      emitRuntimeLog('disconnect:start')
      chat.off?.(eventName, onMessageReceived)
      chat.off?.(sdkReadyEventName, onSdkReady)
      chat.off?.(sdkNotReadyEventName, onSdkNotReady)
      chat.off?.(kickedOutEventName, onKickedOut)
      chat.off?.(friendListUpdatedEventName, onFriendListUpdated)
      sdkReady = false
      loggedInUserId = null
      kickedOut = false
      await chat.logout?.()
      await chat.destroy?.()
      emitRuntimeLog('disconnect:complete')
    },
    async listFriendUserIds() {
      await ensureLoggedIn()
      if (typeof chat.getFriendList !== 'function') {
        emitRuntimeLog('friend-list:unsupported')
        return []
      }
      emitRuntimeLog('friend-list:start')
      try {
        const result = await chat.getFriendList()
        const failure = getTencentImApiFailure('friend-list', result)
        if (failure) throw new Error(failure)
        const userIds = extractTencentImFriendUserIds(result)
        emitRuntimeLog('friend-list:resolved', {
          detail: {
            count: userIds.length
          }
        })
        return userIds
      } catch (err) {
        emitRuntimeLog('friend-list:failed', {
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        throw err
      }
    },
    async sendText(toUserId: string, text: string, options: TencentImSendTextOptions = {}) {
      emitRuntimeLog('send:start', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: {
          sdkReady,
          loginUser: getTencentImLoginUser(chat),
          isReady: typeof chat.isReady === 'function' ? Boolean(chat.isReady()) : null
        }
      })
      await ensureLoggedIn()
      const message = chat.createTextMessage({
        to: toUserId,
        conversationType: TencentCloudChat.TYPES?.CONV_C2C ?? 'C2C',
        payload: { text }
      })
      emitRuntimeLog('send:created', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: summarizeTencentImMessage(message)
      })
      try {
        const result = await chat.sendMessage(message)
        emitRuntimeLog('send:resolved', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: summarizeTencentImApiResult(result)
        })
        const failure = getTencentImApiFailure('send', result)
        if (failure) throw new Error(failure)
      } catch (err) {
        emitRuntimeLog('send:rejected', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        throw err
      }
    },
    async sendImage(toUserId: string, file: File, options: TencentImSendImageOptions = {}) {
      emitRuntimeLog('send:image:start', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: {
          sdkReady,
          loginUser: getTencentImLoginUser(chat),
          isReady: typeof chat.isReady === 'function' ? Boolean(chat.isReady()) : null,
          fileName: file.name,
          fileSize: file.size,
          fileType: file.type
        }
      })
      await ensureLoggedIn()
      const message = chat.createImageMessage({
        to: toUserId,
        conversationType: TencentCloudChat.TYPES?.CONV_C2C ?? 'C2C',
        payload: { file }
      })
      emitRuntimeLog('send:image:created', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: summarizeTencentImMessage(message)
      })
      try {
        const result = await chat.sendMessage(message)
        emitRuntimeLog('send:image:resolved', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: summarizeTencentImApiResult(result)
        })
        const failure = getTencentImApiFailure('send', result)
        if (failure) throw new Error(failure)
      } catch (err) {
        emitRuntimeLog('send:image:rejected', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        throw err
      }
    },
    async sendFile(toUserId: string, file: File, options: TencentImSendFileOptions = {}) {
      emitRuntimeLog('send:file:start', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: {
          sdkReady,
          loginUser: getTencentImLoginUser(chat),
          isReady: typeof chat.isReady === 'function' ? Boolean(chat.isReady()) : null,
          fileName: file.name,
          fileSize: file.size,
          fileType: file.type
        }
      })
      await ensureLoggedIn()
      const message = chat.createFileMessage({
        to: toUserId,
        conversationType: TencentCloudChat.TYPES?.CONV_C2C ?? 'C2C',
        payload: { file }
      })
      emitRuntimeLog('send:file:created', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: summarizeTencentImMessage(message)
      })
      try {
        const result = await chat.sendMessage(message)
        emitRuntimeLog('send:file:resolved', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: summarizeTencentImApiResult(result)
        })
        const failure = getTencentImApiFailure('send', result)
        if (failure) throw new Error(failure)
      } catch (err) {
        emitRuntimeLog('send:file:rejected', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        throw err
      }
    }
  }
}
