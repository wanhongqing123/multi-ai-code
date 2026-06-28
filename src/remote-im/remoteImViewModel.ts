import type {
  RemoteImConfig,
  RemoteImContactRelation,
  RemoteImMessage,
  RemoteImStatus
} from '../../electron/preload.js'

export interface RemoteImContact {
  userId: string
  relation: RemoteImContactRelation
}

export interface RemoteImConversation extends RemoteImContact {
  lastMessagePreview: string | null
  lastMessageAt: number | null
  unreadCount: number
}

export interface RemoteImMessageDisplayMeta {
  userId: string
  relation: RemoteImContactRelation
}

const HIDDEN_REMOTE_IM_MESSAGE_CONTENTS = new Set([
  '已发送给当前 AICLI，开始处理。',
  '操作已完成。'
])

export function getRemoteImStatusLabel(status: RemoteImStatus | null): string {
  if (!status) return '未连接'
  switch (status.state) {
    case 'connected':
      return '已连接'
    case 'connecting':
      return '连接中'
    case 'disabled':
      return '未开启'
    case 'error':
      return '异常'
    case 'disconnected':
    default:
      return '未连接'
  }
}

export function getRemoteImMessageAvatar(message: Pick<RemoteImMessage, 'role'>): string {
  if (message.role === 'remote-user') return '手'
  if (message.role === 'system') return '系'
  return 'AI'
}

export function getRemoteImMessageAuthor(message: RemoteImMessage): string {
  if (message.role === 'remote-user' && message.direction === 'outgoing') return '我'
  if (message.role === 'remote-user') return message.fromUserId || '手机'
  if (message.role === 'system') return 'Multi-AI Code'
  return 'AICLI 输出'
}

export function getRemoteImMessageStatusLabel(message: RemoteImMessage): string {
  switch (message.status) {
    case 'received':
      return ''
    case 'sent-to-aicli':
      return '已发送'
    case 'streaming':
      return '回复中'
    case 'sent-to-im':
      return message.role === 'aicli' ? '已回发' : '已发送'
    case 'rejected':
      return '已拒绝'
    case 'failed':
      return '失败'
    default:
      return ''
  }
}

export function shouldDisplayRemoteImMessage(message: RemoteImMessage): boolean {
  return !HIDDEN_REMOTE_IM_MESSAGE_CONTENTS.has(message.content.trim())
}

function normalizeUserId(userId: string | null | undefined): string | null {
  const value = userId?.trim()
  return value ? value : null
}

function uniqueUserIds(userIds: Array<string | null | undefined>): string[] {
  const seen = new Set<string>()
  const result: string[] = []
  for (const rawUserId of userIds) {
    const userId = normalizeUserId(rawUserId)
    if (!userId || seen.has(userId)) continue
    seen.add(userId)
    result.push(userId)
  }
  return result
}

function addContactRows(
  contacts: RemoteImContact[],
  seen: Set<string>,
  userIds: string[],
  relation: RemoteImContactRelation
): void {
  for (const userId of uniqueUserIds(userIds)) {
    if (seen.has(userId)) continue
    seen.add(userId)
    contacts.push({ userId, relation })
  }
}

export function getRemoteImContacts(config: RemoteImConfig): RemoteImContact[] {
  const contacts: RemoteImContact[] = []
  const seen = new Set<string>()
  addContactRows(contacts, seen, config.friendUserIds, 'friend')
  addContactRows(contacts, seen, config.masterUserIds, 'master')
  addContactRows(contacts, seen, config.slaveUserIds, 'slave')
  return contacts
}

function getRemoteImContactRelation(
  config: RemoteImConfig,
  userId: string
): RemoteImContactRelation {
  if (userId === config.desktopUserId) return config.desktopRole
  return getRemoteImContacts(config).find((contact) => contact.userId === userId)?.relation ?? 'friend'
}

export function getRemoteImMessagePeerUserId(
  message: RemoteImMessage,
  localUserId: string
): string | null {
  const local = normalizeUserId(localUserId)
  const fromUserId = normalizeUserId(message.fromUserId)
  const toUserId = normalizeUserId(message.toUserId)

  if (fromUserId && fromUserId !== local) return fromUserId
  if (toUserId && toUserId !== local) return toUserId
  return message.direction === 'outgoing' ? toUserId || fromUserId : fromUserId || toUserId
}

export function getRemoteImMessageDisplayMeta(
  config: RemoteImConfig,
  message: RemoteImMessage
): RemoteImMessageDisplayMeta {
  const localUserId = normalizeUserId(config.desktopUserId)
  const fromUserId = normalizeUserId(message.fromUserId)
  const toUserId = normalizeUserId(message.toUserId)
  const peerUserId = getRemoteImMessagePeerUserId(message, config.desktopUserId)
  const userId =
    message.direction === 'incoming'
      ? fromUserId ?? peerUserId ?? localUserId
      : fromUserId ?? localUserId ?? toUserId ?? peerUserId

  return {
    userId: userId ?? '',
    relation: userId ? getRemoteImContactRelation(config, userId) : config.desktopRole
  }
}

export function filterRemoteImMessagesByPeer(
  messages: RemoteImMessage[],
  localUserId: string,
  peerUserId: string | null
): RemoteImMessage[] {
  const peer = normalizeUserId(peerUserId)
  if (!peer) return []
  return messages.filter((message) => {
    if (!shouldDisplayRemoteImMessage(message)) return false
    return getRemoteImMessagePeerUserId(message, localUserId) === peer
  })
}

export function getRemoteImConversations(
  config: RemoteImConfig,
  messages: RemoteImMessage[]
): RemoteImConversation[] {
  const conversations = new Map<string, RemoteImConversation>()

  for (const contact of getRemoteImContacts(config)) {
    conversations.set(contact.userId, {
      ...contact,
      lastMessagePreview: null,
      lastMessageAt: null,
      unreadCount: 0
    })
  }

  for (const message of messages) {
    if (!shouldDisplayRemoteImMessage(message)) continue
    const peerUserId = getRemoteImMessagePeerUserId(message, config.desktopUserId)
    if (!peerUserId) continue

    const current =
      conversations.get(peerUserId) ??
      ({
        userId: peerUserId,
        relation: 'friend',
        lastMessagePreview: null,
        lastMessageAt: null,
        unreadCount: 0
      } satisfies RemoteImConversation)

    if (current.lastMessageAt === null || message.createdAt >= current.lastMessageAt) {
      current.lastMessagePreview = message.content.trim()
      current.lastMessageAt = message.createdAt
    }
    if (message.direction === 'incoming' && message.status === 'received') {
      current.unreadCount += 1
    }
    conversations.set(peerUserId, current)
  }

  return Array.from(conversations.values()).sort((left, right) => {
    const leftTime = left.lastMessageAt ?? -1
    const rightTime = right.lastMessageAt ?? -1
    return rightTime - leftTime
  })
}

export function addRemoteImContact(
  config: RemoteImConfig,
  relation: RemoteImContactRelation,
  rawUserId: string
): RemoteImConfig {
  const userId = normalizeUserId(rawUserId)
  if (!userId) return config

  const nextFriendUserIds = uniqueUserIds(config.friendUserIds).filter((item) => item !== userId)
  const nextMasterUserIds = uniqueUserIds(config.masterUserIds).filter((item) => item !== userId)
  const nextSlaveUserIds = uniqueUserIds(config.slaveUserIds).filter((item) => item !== userId)
  const target =
    relation === 'friend'
      ? nextFriendUserIds
      : relation === 'master'
        ? nextMasterUserIds
        : nextSlaveUserIds
  target.push(userId)

  const nextAllowedUserIds = uniqueUserIds([
    ...nextFriendUserIds,
    ...nextMasterUserIds,
    ...nextSlaveUserIds
  ])

  return {
    ...config,
    friendUserIds: nextFriendUserIds,
    masterUserIds: nextMasterUserIds,
    slaveUserIds: nextSlaveUserIds,
    allowedUserIds: nextAllowedUserIds
  }
}

export function formatRemoteImTime(timestamp: number): string {
  const date = new Date(timestamp)
  return `${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(
    2,
    '0'
  )}:${String(date.getSeconds()).padStart(2, '0')}`
}

export function isRemoteImSendDisabled(input: {
  projectId: string | null
  sessionRunning: boolean
  text: string
  status: RemoteImStatus | null
  desktopRole?: 'master' | 'slave'
}): boolean {
  return (
    input.desktopRole === 'slave' ||
    !input.projectId ||
    input.status?.state !== 'connected' ||
    input.text.trim().length === 0
  )
}
