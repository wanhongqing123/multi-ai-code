import type {
  RemoteImConfig,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingFileMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingTextMessage,
  RemoteImApprovalAction,
  RemoteImMessageOrigin,
  RemoteImTextInteraction,
  RemoteImRuntimeLogEntryInput
} from '../../electron/preload.js'

export const REMOTE_IM_CLOUD_METADATA_NAMESPACE = 'multi-ai-code'
export const REMOTE_IM_CLOUD_METADATA_VERSION = 2

interface RemoteImCloudMetadata {
  namespace: typeof REMOTE_IM_CLOUD_METADATA_NAMESPACE
  version: typeof REMOTE_IM_CLOUD_METADATA_VERSION
  origin: RemoteImMessageOrigin
  interaction?: RemoteImTextInteraction
  captionAbove?: true
}

/** Encodes transport metadata shared by Web/Electron and native MaiChat clients. */
export function createRemoteImCloudCustomData(
  origin: RemoteImMessageOrigin,
  interaction?: RemoteImTextInteraction
): string {
  return JSON.stringify({
    namespace: REMOTE_IM_CLOUD_METADATA_NAMESPACE,
    version: REMOTE_IM_CLOUD_METADATA_VERSION,
    origin,
    ...(interaction ? { interaction } : {})
  } satisfies RemoteImCloudMetadata)
}

const APPROVAL_TOKEN_PATTERN = /^approval-[A-Za-z0-9_-]{1,191}$/
const APPROVAL_ACTIONS = new Set<RemoteImApprovalAction>([
  'approve-once',
  'approve-prefix',
  'reject'
])

function parseApprovalAction(value: unknown): RemoteImApprovalAction | undefined {
  return typeof value === 'string' && APPROVAL_ACTIONS.has(value as RemoteImApprovalAction)
    ? (value as RemoteImApprovalAction)
    : undefined
}

function parseRemoteImTextInteraction(
  value: unknown,
  origin: RemoteImMessageOrigin
): RemoteImTextInteraction | undefined {
  if (!value || typeof value !== 'object') return undefined
  const raw = value as Record<string, unknown>
  const token = typeof raw.token === 'string' ? raw.token : ''
  if (!APPROVAL_TOKEN_PATTERN.test(token)) return undefined

  if (raw.kind === 'approval-request' && origin === 'machine') {
    if (!Array.isArray(raw.actions)) return undefined
    const actions = raw.actions.map(parseApprovalAction)
    if (actions.some((action) => !action)) return undefined
    const normalized = actions as RemoteImApprovalAction[]
    if (
      normalized.length < 2 ||
      normalized.length > 3 ||
      new Set(normalized).size !== normalized.length ||
      !normalized.includes('approve-once') ||
      !normalized.includes('reject')
    ) {
      return undefined
    }
    return { kind: 'approval-request', token, actions: normalized }
  }

  if (raw.kind === 'approval-decision' && origin === 'human') {
    const action = parseApprovalAction(raw.action)
    return action ? { kind: 'approval-decision', token, action } : undefined
  }
  if (raw.kind === 'approval-resolved' && origin === 'machine') {
    const outcome = raw.outcome
    if (
      outcome === 'approved' ||
      outcome === 'rejected' ||
      outcome === 'auto-declined' ||
      outcome === 'resolved'
    ) {
      return { kind: 'approval-resolved', token, outcome }
    }
  }
  return undefined
}

export function parseRemoteImCloudMetadata(value: unknown): RemoteImCloudMetadata | undefined {
  if (typeof value !== 'string' || !value.trim()) return undefined
  try {
    const raw = JSON.parse(value) as Record<string, unknown> | null
    if (
      !raw ||
      raw.namespace !== REMOTE_IM_CLOUD_METADATA_NAMESPACE ||
      raw.version !== REMOTE_IM_CLOUD_METADATA_VERSION ||
      (raw.origin !== 'human' && raw.origin !== 'machine')
    ) {
      return undefined
    }
    const origin = raw.origin
    const captionAbove = raw.captionAbove === true ? true : undefined
    const base = {
      namespace: REMOTE_IM_CLOUD_METADATA_NAMESPACE,
      version: REMOTE_IM_CLOUD_METADATA_VERSION,
      origin,
      ...(captionAbove ? { captionAbove } : {})
    } satisfies RemoteImCloudMetadata
    if (raw.interaction === undefined) {
      return base
    }
    const interaction = parseRemoteImTextInteraction(raw.interaction, origin)
    return interaction
      ? { ...base, interaction }
      : undefined
  } catch {
    return undefined
  }
}

/**
 * Reads only this application's versioned metadata. Missing, malformed or
 * foreign custom data deliberately returns undefined so the host can apply its
 * conservative fallback policy.
 */
export function parseRemoteImMessageOrigin(cloudCustomData: unknown): RemoteImMessageOrigin | undefined {
  return parseRemoteImCloudMetadata(cloudCustomData)?.origin
}

function messageMetadata(message: Record<string, unknown>): RemoteImCloudMetadata | undefined {
  return parseRemoteImCloudMetadata(message.cloudCustomData)
}

export interface TencentImTextMessage {
  remoteMessageId: string | null
  fromUserId: string
  toUserId: string | null
  text: string
  origin?: RemoteImMessageOrigin
  interaction?: RemoteImTextInteraction
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
  origin?: RemoteImMessageOrigin
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
  origin?: RemoteImMessageOrigin
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
  origin?: RemoteImMessageOrigin
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

function getTencentImFriendListPayload(payload: unknown): unknown[] | null {
  if (Array.isArray(payload)) return payload
  if (!payload || typeof payload !== 'object') return null
  const raw = payload as Record<string, unknown>
  for (const key of ['data', 'friendList', 'friends', 'list', 'items']) {
    const nested = getTencentImFriendListPayload(raw[key])
    if (nested !== null) return nested
  }
  return null
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
      (getTencentImFriendListPayload(payload) ?? [])
        .map((friend) => getTencentImFriendUserId(friend))
        .filter((userId): userId is string => Boolean(userId))
    )
  )
}

function parseTencentImFriendUserIdsSnapshot(payload: unknown): string[] | null {
  const friends = getTencentImFriendListPayload(payload)
  if (friends === null) return null
  const userIds: string[] = []
  for (const friend of friends) {
    const userId = getTencentImFriendUserId(friend)
    if (!userId) return null
    userIds.push(userId)
  }
  return Array.from(new Set(userIds))
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

function isTencentVideoMessage(message: Record<string, unknown>): boolean {
  const type = typeof message.type === 'string' ? message.type : ''
  // Web SDK 的常量是 TIMVideoFileElem（不是移动端/C SDK 那套 TIMVideoElem）。
  // 三个都认，免得换个发送端就收不到。
  return type === 'TIMVideoFileElem' || type === 'TIMVideoElem' || type === 'MSG_VIDEO'
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
  // 腾讯 Web SDK 的文件元素把地址放在 **fileUrl**（content = { downloadFlag, fileUrl,
  // uuid, fileName, fileSize }），不是图片/语音那样的 url。这里以前只找 url，取不到
  // 就 return null，于是每一条收到的文件都在渲染层被静默丢掉——不进主进程、不落库、
  // 连一条失败回执都没有，发送方看到的是「发送成功」而接收方毫无反应。
  const fileUrl = getStringField(file, [
    'fileUrl',
    'fileURL',
    'url',
    'URL',
    'downloadUrl',
    'downloadURL'
  ])
  if (!fileUrl) return null
  const fileName = getStringField(file, ['fileName', 'filename', 'name'])
  // 不再按 MIME 过滤。以前只放行 md/html，其余类型在这里就被丢掉，用户发来的
  // pdf/zip/docx 会**静默消失**——连一条"收到但不支持"都没有。发送侧
  // （send-file）早就不限类型了，接收侧没跟上是不对称。
  const mimeType =
    getStringField(file, ['mimeType', 'contentType']) ?? mimeTypeFromFileName(fileName)
  return {
    fileUrl,
    sizeBytes: getNumberField(file, ['size', 'dataSize', 'fileSize']),
    uuid: getStringField(file, ['uuid', 'UUID', 'fileId', 'fileID']),
    fileName,
    mimeType
  }
}

interface TencentImElement {
  type: string
  content: unknown
}

// 腾讯 Web SDK 的 Message 只暴露单一 type/payload（取自首个元素），多元素消息（如
// 图片 + 配文）的其余元素藏在私有 _elements 里。这里把整条消息的元素列表取出来，
// 逐元素解析，才能把随图片一起发来的配文一并读到（否则配文在接收层就被丢掉）。
function getTencentImMessageElements(message: Record<string, unknown>): TencentImElement[] {
  const raw = (message as { _elements?: unknown })._elements
  if (Array.isArray(raw)) {
    const list = raw
      .filter((element): element is Record<string, unknown> => Boolean(element && typeof element === 'object'))
      .map((element) => ({
        type: typeof element.type === 'string' ? element.type : '',
        content: (element as { content?: unknown }).content
      }))
    if (list.length > 0) return list
  }
  // 回退：SDK 未暴露 _elements（或为空）时退回单元素（公开的 type/payload）。
  return [
    {
      type: typeof message.type === 'string' ? message.type : '',
      content: (message as { payload?: unknown }).payload
    }
  ]
}

export interface TencentImMessageParts {
  image: Omit<TencentImImageMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null
  file: Omit<TencentImFileMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null
  audio: Omit<TencentImAudioMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null
  /**
   * 视频按「文件」投递：下载到缓存后把本地路径交给 AICLI，与收文件完全同一条链路。
   * 单独留一个字段而不是直接塞进 file，只是为了运行日志能把两者分开——
   * 排障时「收到 3 个文件」和「收到 1 个视频 2 个文件」是两回事。
   */
  video: Omit<TencentImFileMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null
  caption: string | null
  /** false = attachment then caption; true = caption then attachment. Caption presence is `caption`. */
  captionAbove: boolean
}

const VIDEO_MIME_BY_FORMAT: Record<string, string> = {
  mov: 'video/quicktime',
  mp4: 'video/mp4',
  quicktime: 'video/quicktime'
}

/**
 * 把收到的视频元素映射成文件形态。视频元素不带文件名，扩展名只能从 videoFormat 推；
 * 一个都推不出来时按 mp4 兜底——SDK 本来就只收 mp4/mov，落成 .bin 反而让接收方
 * 拿到一个双击打不开的文件。
 */
function getVideoAsFilePayload(
  message: Record<string, unknown>
): Omit<TencentImFileMessage, 'remoteMessageId' | 'fromUserId' | 'toUserId' | 'createdAt'> | null {
  if (!isTencentVideoMessage(message)) return null
  const payload = message.payload
  if (!payload || typeof payload !== 'object') return null
  const video = payload as Record<string, unknown>
  const fileUrl = getStringField(video, ['remoteVideoUrl', 'videoUrl', 'url', 'URL'])
  if (!fileUrl) return null
  const uuid = getStringField(video, ['videoUUID', 'videoUuid', 'uuid', 'UUID'])
  const format = (getStringField(video, ['videoFormat', 'format']) ?? 'mp4').toLowerCase()
  const extension = /^[a-z0-9]{1,8}$/.test(format) ? format : 'mp4'
  return {
    fileUrl,
    sizeBytes: getNumberField(video, ['videoSize', 'size', 'fileSize']),
    uuid,
    fileName: `${uuid || 'remote-im-video'}.${extension}`,
    mimeType: VIDEO_MIME_BY_FORMAT[format] ?? `video/${extension}`
  }
}

// 把一条（可能多元素的）消息拆成：首个图片/文件/语音附件 + 首条非空文本（作为附件配文）。
// 复用既有的单元素 payload 解析器：为每个元素构造 { type, payload } 伪消息喂进去。
export function extractTencentImMessageParts(message: Record<string, unknown>): TencentImMessageParts {
  const parts: TencentImMessageParts = {
    image: null,
    file: null,
    audio: null,
    video: null,
    caption: null,
    captionAbove: false
  }
  const elements = getTencentImMessageElements(message)
  for (let index = 0; index < elements.length; index += 1) {
    const element = elements[index]
    const pseudo = { ...message, type: element.type, payload: element.content }
    if (!parts.image) {
      const image = getImagePayload(pseudo)
      if (image) {
        parts.image = image
        continue
      }
    }
    if (!parts.file) {
      const file = getFilePayload(pseudo)
      if (file) {
        parts.file = file
        continue
      }
    }
    if (!parts.audio) {
      const audio = getAudioPayload(pseudo)
      if (audio) {
        parts.audio = audio
        continue
      }
    }
    if (!parts.video) {
      const video = getVideoAsFilePayload(pseudo)
      if (video) {
        parts.video = video
        continue
      }
    }
    if (parts.caption === null) {
      const text = getTextPayload(pseudo)
      if (text) {
        parts.caption = text
      }
    }
  }
  parts.captionAbove = parts.caption !== null && messageMetadata(message)?.captionAbove === true
  return parts
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
    const metadata = messageMetadata(message)
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        text,
        ...(metadata?.origin ? { origin: metadata.origin } : {}),
        ...(metadata?.interaction ? { interaction: metadata.interaction } : {}),
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

export interface TencentImRoamedTextMessage {
  remoteMessageId: string
  fromUserId: string
  toUserId: string | null
  text: string
  origin?: RemoteImMessageOrigin
  createdAt: number | undefined
  flow: 'in' | 'out'
}

// 漫游历史（getMessageList 返回的 messageList）里的文本消息：与实时事件不同，
// 双向消息（flow in/out）都在，且必须带 SDK 消息 ID 才能与已入库的消息去重——
// 没有 ID 的条目直接丢弃（无法安全合并）。
export function extractTencentImRoamedTextMessages(
  messageList: unknown
): TencentImRoamedTextMessage[] {
  if (!Array.isArray(messageList)) return []
  return messageList.flatMap((item) => {
    if (!item || typeof item !== 'object') return []
    const message = item as Record<string, unknown>
    const text = getTextPayload(message)
    if (!text) return []
    const remoteMessageId = typeof message.ID === 'string' && message.ID.trim() ? message.ID : null
    if (!remoteMessageId) return []
    const from = typeof message.from === 'string' ? message.from : ''
    if (!from) return []
    const flow = message.flow === 'out' ? 'out' : 'in'
    const origin = messageMetadata(message)?.origin
    return [
      {
        remoteMessageId,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        text,
        ...(origin ? { origin } : {}),
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined,
        flow
      } as TencentImRoamedTextMessage
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
    const origin = messageMetadata(message)?.origin
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        ...image,
        ...(origin ? { origin } : {}),
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
    const origin = messageMetadata(message)?.origin
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        ...file,
        ...(origin ? { origin } : {}),
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
    const origin = messageMetadata(message)?.origin
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        ...audio,
        ...(origin ? { origin } : {}),
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

/**
 * 按账号配置取 userSig：secret-key 本地算，endpoint 走凭证接口。
 *
 * 导出是给远程桌面用的：TRTC 和 IM 同 sdkAppId、同 userId，签名必须
 * 用同一套规则算出来。远程桌面那边原先无条件本地生成，账号是 endpoint
 * 模式时 secretKey 为空，直接抛「内置连接凭证无效」。
 */
export async function requestUserSig(config: RemoteImConfig): Promise<string> {
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
  /** Defaults to machine so an unclassified programmatic sender cannot create an auto-reply loop. */
  origin?: RemoteImMessageOrigin
  interaction?: RemoteImTextInteraction
}

export type TencentImSendImageOptions = TencentImSendTextOptions
export type TencentImSendFileOptions = TencentImSendTextOptions
export type TencentImSendVideoOptions = TencentImSendTextOptions

export interface TencentImSendResult {
  remoteMessageId: string | null
}

export interface TencentImRuntime {
  disconnect(): Promise<void>
  listFriendUserIds?(): Promise<string[]>
  // 登录后补拉各 C2C 会话的漫游文本消息（离线期间的消息只能从这里拿到），
  // 供宿主按 remoteMessageId 去重后补入本地消息库。
  listRoamedTextMessages?(options?: {
    perConversationCount?: number
  }): Promise<TencentImRoamedTextMessage[]>
  sendText(
    toUserId: string,
    text: string,
    options?: TencentImSendTextOptions
  ): Promise<TencentImSendResult | void>
  sendImage?(
    toUserId: string,
    file: File,
    options?: TencentImSendImageOptions
  ): Promise<TencentImSendResult | void>
  sendFile?(
    toUserId: string,
    file: File,
    options?: TencentImSendFileOptions
  ): Promise<TencentImSendResult | void>
  sendVideo?(
    toUserId: string,
    file: File,
    options?: TencentImSendVideoOptions
  ): Promise<TencentImSendResult | void>
}

/** 视频时长探测的硬超时。探不出来不该拖住整条发送链路。 */
const VIDEO_DURATION_PROBE_TIMEOUT_MS = 3_000

/**
 * 用 <video> 读一次 metadata 拿时长（秒）。腾讯 IM Web SDK 直接从 File 上读
 * `duration` 字段，而浏览器的 File 根本没有这个字段——不补的话对端气泡永远显示 0 秒。
 * 任何一步失败都返回 0：时长只是展示信息，不值得让发送失败。
 */
export async function probeVideoDurationSeconds(file: File): Promise<number> {
  try {
    if (typeof document === 'undefined') return 0
    if (typeof URL === 'undefined' || typeof URL.createObjectURL !== 'function') return 0
    const objectUrl = URL.createObjectURL(file)
    return await new Promise<number>((resolve) => {
      let settled = false
      const element = document.createElement('video')
      const finish = (seconds: number) => {
        if (settled) return
        settled = true
        clearTimeout(timer)
        element.removeAttribute('src')
        URL.revokeObjectURL(objectUrl)
        resolve(Number.isFinite(seconds) && seconds > 0 ? Math.round(seconds) : 0)
      }
      const timer = setTimeout(() => finish(0), VIDEO_DURATION_PROBE_TIMEOUT_MS)
      element.preload = 'metadata'
      element.onloadedmetadata = () => finish(element.duration)
      element.onerror = () => finish(0)
      element.src = objectUrl
    })
  } catch {
    return 0
  }
}

/** 把探到的时长挂到 File 上供 SDK 读取；挂不上就原样返回（SDK 落到 0 秒）。 */
export function withProbedVideoDuration(file: File, durationSeconds: number): File {
  if (!(durationSeconds > 0)) return file
  try {
    Object.defineProperty(file, 'duration', {
      value: durationSeconds,
      configurable: true,
      enumerable: false,
      writable: true
    })
  } catch {
    // 只读/密封的 File 实现：交给 SDK 的 0 秒兜底。
  }
  return file
}

// sendMessage resolve 结果里带服务端确认的消息（含 ID）。取出来回填到本地
// 出站记录，漫游重投同一条消息时才能按 remote_message_id 去重。
export function getSentRemoteMessageId(result: unknown): string | null {
  if (!result || typeof result !== 'object') return null
  const message = (result as { data?: { message?: unknown } }).data?.message
  if (!message || typeof message !== 'object') return null
  const id = (message as { ID?: unknown }).ID
  return typeof id === 'string' && id.trim() ? id : null
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
    const data = event && typeof event === 'object' ? (event as { data?: unknown }).data : null
    if (!Array.isArray(data)) return

    let textCount = 0
    let audioCount = 0
    let imageCount = 0
    let fileCount = 0
    let videoCount = 0

    for (const item of data) {
      if (!item || typeof item !== 'object') continue
      const message = item as Record<string, unknown>
      const fromUserId = typeof message.from === 'string' ? message.from : ''
      if (!fromUserId) continue
      const remoteMessageId = typeof message.ID === 'string' ? message.ID : null
      const toUserId = typeof message.to === 'string' ? message.to : null
      const createdAt = typeof message.time === 'number' ? message.time * 1000 : undefined
      const metadata = messageMetadata(message)
      const origin = metadata?.origin

      // 逐条消息按元素拆解：附件（图片/文件/语音）与配文来自「同一条」消息，
      // 图片 + 配文合并成一次 AICLI 投递。
      const parts = extractTencentImMessageParts(message)

      if (parts.image) {
        imageCount++
        input.onIncomingImage?.({
          projectId: input.projectId,
          remoteMessageId,
          fromUserId,
          toUserId,
          imageUrl: parts.image.imageUrl,
          thumbnailUrl: parts.image.thumbnailUrl,
          width: parts.image.width,
          height: parts.image.height,
          sizeBytes: parts.image.sizeBytes,
          uuid: parts.image.uuid,
          fileName: parts.image.fileName,
          mimeType: parts.image.mimeType,
          caption: parts.caption,
          captionAbove: parts.captionAbove,
          ...(origin ? { origin } : {}),
          createdAt
        })
        continue
      }

      // 视频与文件走同一条投递链路：下载到缓存 → 把本地路径交给 AICLI。
      // 以前视频在这里既不匹配 image 也不匹配 file，四个回调一个都不触发，
      // 整条消息静默消失；带配文时更糟——只有文字进了 AICLI，视频没了，
      // 用户看到 AICLI 回应了他的话，以为视频收到了。
      const fileLike = parts.file ?? parts.video
      if (fileLike) {
        if (parts.file) fileCount++
        else videoCount++
        // 配文与文件合并成一次投递，与图片一致：拆成两条会让 AICLI
        // 把用户的一条消息当成两个独立任务。
        input.onIncomingFile?.({
          projectId: input.projectId,
          remoteMessageId,
          fromUserId,
          toUserId,
          fileUrl: fileLike.fileUrl,
          sizeBytes: fileLike.sizeBytes,
          uuid: fileLike.uuid,
          fileName: fileLike.fileName,
          mimeType: fileLike.mimeType,
          caption: parts.caption,
          captionAbove: parts.captionAbove,
          ...(origin ? { origin } : {}),
          createdAt
        })
        continue
      }

      if (parts.audio) {
        audioCount++
        input.onIncomingAudio?.({
          projectId: input.projectId,
          remoteMessageId,
          fromUserId,
          toUserId,
          audioUrl: parts.audio.audioUrl,
          durationSeconds: parts.audio.durationSeconds,
          sizeBytes: parts.audio.sizeBytes,
          uuid: parts.audio.uuid,
          ...(origin ? { origin } : {}),
          createdAt
        })
        continue
      }

      if (parts.caption) {
        textCount++
        input.onIncomingText({
          projectId: input.projectId,
          remoteMessageId,
          fromUserId,
          toUserId,
          text: parts.caption,
          ...(origin ? { origin } : {}),
          ...(metadata?.interaction ? { interaction: metadata.interaction } : {}),
          createdAt
        })
      }
    }

    emitRuntimeLog('message:received', {
      detail: {
        count: textCount,
        audioCount,
        imageCount,
        fileCount,
        videoCount
      }
    })
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
    // The event is a refresh signal, not necessarily a complete snapshot.
    // Notify even when its payload is empty so the host can call
    // getFriendList() and persist an authoritative empty list.
    input.onFriendListUpdated?.(userIds)
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
        // An unavailable API is not an authoritative empty snapshot. Reject so
        // the account keeps its last known allow-list instead of revoking every
        // contact because this SDK build cannot enumerate friends.
        throw new Error('Tencent IM friend-list API is unavailable')
      }
      emitRuntimeLog('friend-list:start')
      try {
        const result = await chat.getFriendList()
        const failure = getTencentImApiFailure('friend-list', result)
        if (failure) throw new Error(failure)
        const userIds = parseTencentImFriendUserIdsSnapshot(result)
        if (userIds === null) {
          throw new Error('Tencent IM returned a malformed friend-list snapshot')
        }
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
    async listRoamedTextMessages(options = {}) {
      await ensureLoggedIn()
      if (typeof chat.getConversationList !== 'function' || typeof chat.getMessageList !== 'function') {
        emitRuntimeLog('roam:unsupported')
        return []
      }
      const perConversationCount = options.perConversationCount ?? 15
      emitRuntimeLog('roam:start')
      const roamed: TencentImRoamedTextMessage[] = []
      try {
        const conversationsResult = await chat.getConversationList()
        const conversationList =
          (conversationsResult as { data?: { conversationList?: unknown } })?.data
            ?.conversationList ?? []
        const c2cType = TencentCloudChat.TYPES?.CONV_C2C ?? 'C2C'
        for (const item of Array.isArray(conversationList) ? conversationList : []) {
          if (!item || typeof item !== 'object') continue
          const conversation = item as { conversationID?: unknown; type?: unknown }
          if (conversation.type !== c2cType) continue
          const conversationID =
            typeof conversation.conversationID === 'string' ? conversation.conversationID : ''
          if (!conversationID) continue
          try {
            const listResult = await chat.getMessageList({
              conversationID,
              count: perConversationCount
            })
            const messageList =
              (listResult as { data?: { messageList?: unknown } })?.data?.messageList ?? []
            roamed.push(...extractTencentImRoamedTextMessages(messageList))
          } catch (err) {
            // 单个会话拉取失败不阻断其他会话的补拉。
            emitRuntimeLog('roam:conversation-failed', {
              detail: {
                conversationID,
                error: err instanceof Error ? err.message : String(err)
              }
            })
          }
        }
        emitRuntimeLog('roam:resolved', { detail: { count: roamed.length } })
        return roamed
      } catch (err) {
        emitRuntimeLog('roam:failed', {
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        return roamed
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
        payload: { text },
        cloudCustomData: createRemoteImCloudCustomData(
          options.origin ?? 'machine',
          options.interaction
        )
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
        return { remoteMessageId: getSentRemoteMessageId(result) }
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
        payload: { file },
        cloudCustomData: createRemoteImCloudCustomData(options.origin ?? 'machine')
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
        return { remoteMessageId: getSentRemoteMessageId(result) }
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
        payload: { file },
        cloudCustomData: createRemoteImCloudCustomData(options.origin ?? 'machine')
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
        return { remoteMessageId: getSentRemoteMessageId(result) }
      } catch (err) {
        emitRuntimeLog('send:file:rejected', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        throw err
      }
    },
    async sendVideo(toUserId: string, file: File, options: TencentImSendVideoOptions = {}) {
      // 时长只影响对端气泡上显示的秒数；探测失败就交给 SDK 落到 0，别因此拦下发送。
      const durationSeconds = await probeVideoDurationSeconds(file)
      emitRuntimeLog('send:video:start', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: {
          sdkReady,
          loginUser: getTencentImLoginUser(chat),
          isReady: typeof chat.isReady === 'function' ? Boolean(chat.isReady()) : null,
          fileName: file.name,
          fileSize: file.size,
          fileType: file.type,
          durationSeconds
        }
      })
      await ensureLoggedIn()
      const message = chat.createVideoMessage({
        to: toUserId,
        conversationType: TencentCloudChat.TYPES?.CONV_C2C ?? 'C2C',
        payload: { file: withProbedVideoDuration(file, durationSeconds) },
        cloudCustomData: createRemoteImCloudCustomData(options.origin ?? 'machine')
      })
      emitRuntimeLog('send:video:created', {
        peerUserId: toUserId,
        messageId: options.messageId,
        detail: summarizeTencentImMessage(message)
      })
      try {
        const result = await chat.sendMessage(message)
        emitRuntimeLog('send:video:resolved', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: summarizeTencentImApiResult(result)
        })
        const failure = getTencentImApiFailure('send', result)
        if (failure) throw new Error(failure)
        return { remoteMessageId: getSentRemoteMessageId(result) }
      } catch (err) {
        emitRuntimeLog('send:video:rejected', {
          peerUserId: toUserId,
          messageId: options.messageId,
          detail: { error: err instanceof Error ? err.message : String(err) }
        })
        throw err
      }
    }
  }
}
