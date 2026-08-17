import type {
  RemoteImConfig,
  RemoteImContactRelation,
  RemoteImMessage,
  RemoteImStatus
} from '../../electron/preload.js'

export interface RemoteImContact {
  userId: string
  relation: RemoteImContactRelation
  /**
   * 是否真的在联系人配置里。
   *
   * 单靠 relation 判断不出来：它是持久化类型（friend/master/slave），没有
   * "不是联系人"这一档，查不到时只能兜个默认值——以前兜的是 'friend'，
   * 于是任何给你发过消息的陌生账号都被显示成好友，而它的消息同时标着
   * 「已拒绝」。授权判定读的是配置，界面读的是兜底值，两边说的不是一回事。
   */
  isContact: boolean
}

export interface RemoteImConversation extends RemoteImContact {
  lastMessagePreview: string | null
  lastMessageAt: number | null
  unreadCount: number
}

export interface RemoteImMessageDisplayMeta {
  userId: string
  relation: RemoteImContactRelation
  isContact: boolean
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
      return '未连接'
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
      return '✓'
    case 'streaming':
      return '回复中'
    case 'sent-to-im':
      return '✓'
    case 'rejected':
      return '已拒绝'
    case 'failed':
      return '失败'
    default:
      return ''
  }
}

export function getRemoteImMessageStatusTitle(message: RemoteImMessage): string {
  switch (message.status) {
    case 'sent-to-aicli':
      return '已发送'
    case 'sent-to-im':
      return message.role === 'aicli' ? '已回发' : '已发送'
    case 'streaming':
      return '回复中'
    case 'rejected':
      return '已拒绝'
    case 'failed':
      return '失败'
    case 'received':
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
    // 这个函数只从配置里取人，所以一律是真联系人。
    contacts.push({ userId, relation, isContact: true })
  }
}

export function getRemoteImContacts(config: RemoteImConfig): RemoteImContact[] {
  const contacts: RemoteImContact[] = []
  const seen = new Set<string>()
  addContactRows(contacts, seen, config.friendUserIds, 'friend')
  addContactRows(contacts, seen, config.masterUserIds, 'friend')
  addContactRows(contacts, seen, config.slaveUserIds, 'friend')
  addContactRows(contacts, seen, config.allowedUserIds, 'friend')
  return contacts
}

function getRemoteImContactRelation(
  config: RemoteImConfig,
  userId: string
): RemoteImContactRelation {
  if (userId === config.desktopUserId) return 'friend'
  return getRemoteImContacts(config).find((contact) => contact.userId === userId)?.relation ?? 'friend'
}

/** 是不是真在联系人配置里。自己永远算"是"。 */
export function isRemoteImContact(config: RemoteImConfig, userId: string): boolean {
  if (!userId) return false
  if (userId === config.desktopUserId) return true
  return getRemoteImContacts(config).some((contact) => contact.userId === userId)
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
    relation: userId ? getRemoteImContactRelation(config, userId) : config.desktopRole,
    isContact: userId ? isRemoteImContact(config, userId) : true
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
      // 只在消息里出现过、配置里没有的人：显示成陌生人。
      // 以前这里兜底成 'friend'，未授权账号会混进「好友」页。
      ({
        userId: peerUserId,
        relation: 'friend',
        isContact: false,
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
  void relation
  nextFriendUserIds.push(userId)

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

export function removeRemoteImContact(
  config: RemoteImConfig,
  rawUserId: string
): RemoteImConfig {
  const userId = normalizeUserId(rawUserId)
  if (!userId) return config

  const nextFriendUserIds = uniqueUserIds(config.friendUserIds).filter((item) => item !== userId)
  const nextMasterUserIds = uniqueUserIds(config.masterUserIds).filter((item) => item !== userId)
  const nextSlaveUserIds = uniqueUserIds(config.slaveUserIds).filter((item) => item !== userId)
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
  void input.desktopRole
  return (
    !input.projectId ||
    input.status?.state !== 'connected' ||
    input.text.trim().length === 0
  )
}
