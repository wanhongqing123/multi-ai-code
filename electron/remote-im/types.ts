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
}

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
export type RemoteImMessageStatus =
  | 'received'
  | 'rejected'
  | 'sent-to-aicli'
  | 'streaming'
  | 'sent-to-im'
  | 'failed'

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
  status: RemoteImMessageStatus
  error: string | null
  createdAt: number
  sentToAicliAt: number | null
  sentToImAt: number | null
}

export interface RemoteImIncomingTextMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  text: string
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
