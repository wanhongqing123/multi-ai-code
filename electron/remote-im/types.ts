export type RemoteImProvider = 'tencent-im'

export interface RemoteImConfig {
  enabled: boolean
  provider: RemoteImProvider
  sdkAppId: number | null
  desktopUserId: string
  userSigEndpoint: string
  allowedUserIds: string[]
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
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

export interface RemoteImValidationIssue {
  path: keyof RemoteImConfig
  message: string
}

export type RemoteImValidationResult =
  | { ok: true; issues: [] }
  | { ok: false; issues: RemoteImValidationIssue[] }
