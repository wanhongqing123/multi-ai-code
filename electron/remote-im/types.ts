export type RemoteImProvider = 'tencent-im'
export type RemoteImContactRelation = 'friend' | 'master' | 'slave'

export interface RemoteImConfig {
  provider: RemoteImProvider
  sdkAppId: number | null
  desktopUserId: string
  userSigMode: 'endpoint' | 'secret-key'
  userSigEndpoint: string
  userSigSecretKey: string
  friendUserIds: string[]
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
  /**
   * 被控端远程桌面模式。默认 disabled——这台机器上跑着 AICLI 和你的仓库，
   * 屏幕共享必须由用户显式开一次，不能因为装了新版本就默认可被查看。
   * 谁能连入只看一份名单：IM 好友（friendUserIds）。本地删掉的好友在库里留墓碑，
   * 查不出来即等于禁止，不需要另一份「允许名单」。
   */
  remoteDesktopMode: RemoteDesktopMode
  /**
   * 是否允许对端操作本机键鼠。独立于 remoteDesktopMode：
   * 开了"看屏幕"不等于把整台电脑交出去，必须再单独授权一次。
   */
  remoteDesktopControl: boolean
}

export type RemoteDesktopMode = 'disabled' | 'attended' | 'unattended'

export interface RemoteImAccountConfig {
  provider: RemoteImProvider
  sdkAppId: number | null
  desktopUserId: string
  userSigMode: 'endpoint' | 'secret-key'
  userSigEndpoint: string
  userSigSecretKey: string
  // 以下四项以前存在每个项目的 project.json 里，但它们描述的是「这台机器上的这个
  // 账号」怎么工作，与具体仓库无关，分项目存只会让同一台机器上出现互相矛盾的设置。
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
  remoteDesktopMode: RemoteDesktopMode
  remoteDesktopControl: boolean
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
export type RemoteImMessageKind = 'text' | 'image' | 'file' | 'video'
/**
 * Who caused a Remote IM message to be sent.
 *
 * This is transport metadata, not a display role: a machine-originated message
 * is still delivered to AICLI, but the host must not automatically forward that
 * turn's output back to IM.
 */
export type RemoteImMessageOrigin = 'human' | 'machine'

export interface RemoteImGitDiffArtifactSource {
  kind: 'working' | 'commit' | 'range'
  label: string
  requestedRef?: string
  requestedBase?: string
  requestedHead?: string
  baseOid?: string
  headOid: string
}

export interface RemoteImGitDiffArtifact {
  schema: 'git-diff/v1'
  id: string
  repositoryName: string
  source: RemoteImGitDiffArtifactSource
  files: number
  additions: number
  deletions: number
  sha256: string
  sizeBytes: number
  complete: boolean
}

export type RemoteImApprovalAction = 'approve-once' | 'approve-prefix' | 'reject'

export interface RemoteImApprovalRequestInteraction {
  kind: 'approval-request'
  token: string
  actions: RemoteImApprovalAction[]
}

export interface RemoteImApprovalDecisionInteraction {
  kind: 'approval-decision'
  token: string
  action: RemoteImApprovalAction
}

export type RemoteImApprovalResolutionOutcome =
  | 'approved'
  | 'rejected'
  | 'auto-declined'
  | 'resolved'

export interface RemoteImApprovalResolvedInteraction {
  kind: 'approval-resolved'
  token: string
  outcome: RemoteImApprovalResolutionOutcome
}

export type RemoteImTextInteraction =
  | RemoteImApprovalRequestInteraction
  | RemoteImApprovalDecisionInteraction
  | RemoteImApprovalResolvedInteraction
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

/**
 * 视频消息附件。封面(thumbnail)由 IM 服务端在上传后生成，本端发送时拿不到，
 * 所以 thumbnailUrl/width/height 出站阶段一律为 null，等回执或对端消息才有值。
 */
export interface RemoteImVideoAttachment {
  type: 'video'
  localPath: string | null
  remoteUrl: string | null
  thumbnailUrl: string | null
  width: number | null
  height: number | null
  durationSeconds: number | null
  sizeBytes: number | null
  fileName: string | null
  mimeType: string | null
  sdkVideoId: string | null
}

export type RemoteImMessageAttachment =
  | RemoteImImageAttachment
  | RemoteImFileAttachment
  | RemoteImVideoAttachment

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
  caption?: string | null
  /** Missing/false = attachment first; true = caption first. Caption presence is `caption`. */
  captionAbove?: boolean
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
  /** Versioned first-party interaction. Invalid or foreign wire data is discarded. */
  interaction?: RemoteImTextInteraction
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
  captionAbove?: boolean
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
  captionAbove?: boolean
  /** Missing/invalid wire metadata is intentionally left undefined for the host policy. */
  origin?: RemoteImMessageOrigin
  artifact?: RemoteImGitDiffArtifact
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
