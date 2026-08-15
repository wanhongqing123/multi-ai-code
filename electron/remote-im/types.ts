export type RemoteImProvider = 'tencent-im'
export type RemoteImDesktopRole = 'master' | 'slave'
export type RemoteImContactRelation = 'friend' | 'master' | 'slave'

export interface RemoteImConfig {
  enabled: boolean
  provider: RemoteImProvider
  sdkAppId: number | null
  desktopUserId: string
  desktopRole: RemoteImDesktopRole
  userSigMode: 'endpoint' | 'secret-key'
  userSigEndpoint: string
  userSigSecretKey: string
  friendUserIds: string[]
  masterUserIds: string[]
  slaveUserIds: string[]
  allowedUserIds: string[]
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
  /**
   * 被控端远程桌面模式。默认 disabled——这台机器上跑着 AICLI 和你的仓库，
   * 屏幕共享必须由用户显式开一次，不能因为装了新版本就默认可被查看。
   * 允许连入的账号复用 allowedUserIds（IM 好友白名单）。
   */
  remoteDesktopMode: RemoteDesktopMode
}

export type RemoteDesktopMode = 'disabled' | 'attended' | 'unattended'

export interface RemoteImAccountConfig {
  provider: RemoteImProvider
  sdkAppId: number | null
  desktopUserId: string
  desktopRole: RemoteImDesktopRole
  userSigMode: 'endpoint' | 'secret-key'
  userSigEndpoint: string
  userSigSecretKey: string
  friendUserIds: string[]
  masterUserIds: string[]
  slaveUserIds: string[]
  allowedUserIds: string[]
  /** Locally revoked SDK friends. Omitted by older account files. */
  blockedUserIds?: string[]
}

export interface RemoteImLoginState {
  profileId: string | null
  account: RemoteImAccountConfig
}

export type RemoteImConnectionState =
  | 'disabled'
  | 'disconnected'
  | 'connecting'
  | 'connected'
  | 'error'

export interface RemoteImStatus {
  projectId: string | null
  state: RemoteImConnectionState
  detail: string | null
  updatedAt: number
}

export type RemoteImMessageRole = 'remote-user' | 'system' | 'aicli'
export type RemoteImMessageDirection = 'incoming' | 'outgoing' | 'internal'
export type RemoteImMessageKind = 'text' | 'image' | 'file'
/**
 * Who caused a Remote IM message to be sent.
 *
 * This is transport metadata, not a display role: a machine-originated message
 * is still delivered to AICLI, but the host must not automatically forward that
 * turn's output back to IM.
 */
export type RemoteImMessageOrigin = 'human' | 'machine'
export type RemoteImMessageStatus =
  | 'received'
  | 'rejected'
  | 'sent-to-aicli'
  | 'streaming'
  | 'sent-to-im'
  | 'failed'

export interface RemoteImImageAttachment {
  type: 'image'
  localPath: string | null
  remoteUrl: string | null
  thumbnailUrl: string | null
  width: number | null
  height: number | null
  sizeBytes: number | null
  fileName: string | null
  mimeType: string | null
  sdkImageId: string | null
}

export interface RemoteImFileAttachment {
  type: 'file'
  localPath: string | null
  remoteUrl: string | null
  sizeBytes: number | null
  fileName: string | null
  mimeType: string | null
  sdkFileId: string | null
}

export type RemoteImMessageAttachment = RemoteImImageAttachment | RemoteImFileAttachment

export interface RemoteImMessage {
  id: number
  projectId: string | null
  sessionId: string | null
  provider: RemoteImProvider
  remoteMessageId: string | null
  fromUserId: string | null
  toUserId: string | null
  role: RemoteImMessageRole
  direction: RemoteImMessageDirection
  content: string
  kind: RemoteImMessageKind
  attachment: RemoteImMessageAttachment | null
  status: RemoteImMessageStatus
  error: string | null
  createdAt: number
  sentToAicliAt: number | null
  sentToImAt: number | null
}

export interface ReadRemoteImImagePreviewInput {
  projectId: string
  messageId: number
}

export type ReadRemoteImImagePreviewResult =
  | { ok: true; dataUrl: string }
  | { ok: false; error: string }

// SDK 漫游补拉的文本消息（登录后补充离线期间的历史，只入库展示、不路由 AICLI）。
export interface RemoteImRoamedTextMessage {
  remoteMessageId: string
  fromUserId: string
  toUserId?: string | null
  text: string
  origin?: RemoteImMessageOrigin
  createdAt?: number
  flow: 'in' | 'out'
}

/** Identifies one concrete renderer-side Tencent IM connection lifecycle. */
export interface RemoteImRuntimeIdentity {
  connectionId: string
  desktopUserId: string
  sdkAppId: number | null
}

export interface RemoteImIncomingTextMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  text: string
  /** Missing/invalid wire metadata is intentionally left undefined for the host policy. */
  origin?: RemoteImMessageOrigin
  createdAt?: number
}

export interface RemoteImIncomingAudioMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  audioUrl: string
  durationSeconds?: number | null
  sizeBytes?: number | null
  uuid?: string | null
  /** Missing/invalid wire metadata is intentionally left undefined for the host policy. */
  origin?: RemoteImMessageOrigin
  createdAt?: number
}

export interface RemoteImIncomingImageMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  imageUrl: string
  thumbnailUrl?: string | null
  width?: number | null
  height?: number | null
  sizeBytes?: number | null
  uuid?: string | null
  fileName?: string | null
  mimeType?: string | null
  // 同一条多元素消息里随图片一起发来的配文。图片下载后与配文合并成「一次」AICLI 输入。
  caption?: string | null
  /** Missing/invalid wire metadata is intentionally left undefined for the host policy. */
  origin?: RemoteImMessageOrigin
  createdAt?: number
}

export interface RemoteImIncomingFileMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  fileUrl: string
  sizeBytes?: number | null
  uuid?: string | null
  fileName?: string | null
  mimeType?: string | null
  // 同一条多元素消息里随文件一起发来的配文，与图片同样合并成「一次」AICLI 输入。
  caption?: string | null
  /** Missing/invalid wire metadata is intentionally left undefined for the host policy. */
  origin?: RemoteImMessageOrigin
  createdAt?: number
}

export interface RemoteImRuntimeLogEntryInput {
  projectId?: string | null
  sdkAppId?: number | null
  desktopUserId?: string | null
  peerUserId?: string | null
  messageId?: number | null
  event: string
  detail?: unknown
  createdAt?: number
}

export interface RemoteImRuntimeLogEntry {
  projectId: string | null
  sdkAppId: number | null
  desktopUserId: string | null
  peerUserId: string | null
  messageId: number | null
  event: string
  detail: unknown
  createdAt: number
}

export interface RemoteImValidationIssue {
  path: keyof RemoteImConfig
  message: string
}

export type RemoteImValidationResult =
  | { ok: true; issues: [] }
  | { ok: false; issues: RemoteImValidationIssue[] }
