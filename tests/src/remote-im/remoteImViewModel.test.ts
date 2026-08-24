import { describe, expect, it } from 'vitest'
import type { RemoteImConfig, RemoteImMessage, RemoteImStatus } from '../../../electron/preload.js'
import {
  addRemoteImContact,
  filterRemoteImMessagesByPeer,
  getRemoteImContacts,
  getRemoteImConversations,
  getRemoteImMessageDisplayMeta,
  getRemoteImMessageAvatar,
  getRemoteImMessageStatusLabel,
  getRemoteImStatusLabel,
  isRemoteImContact,
  isRemoteImSendDisabled,
  removeRemoteImContact
} from '../../../src/remote-im/remoteImViewModel.js'

const config: RemoteImConfig = {
  provider: 'tencent-im',
  sdkAppId: 1400000000,
  desktopUserId: 'local-user',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: ['friend-a', 'master-a', 'slave-a'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200,
  remoteDesktopMode: 'disabled' as const,
  remoteDesktopControl: false
}

function message(overrides: Partial<RemoteImMessage>): RemoteImMessage {
  return {
    id: 1,
    projectId: 'project-1',
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: null,
    toUserId: null,
    role: 'remote-user',
    direction: 'incoming',
    content: 'hello',
    kind: 'text',
    attachment: null,
    status: 'received',
    error: null,
    createdAt: 100,
    sentToAicliAt: null,
    sentToImAt: null,
    ...overrides
  }
}

describe('remote IM view model', () => {
  it('maps connection status to short labels', () => {
    const status: RemoteImStatus = {
      projectId: 'project-1',
      state: 'connected',
      detail: null,
      updatedAt: 1
    }
    expect(getRemoteImStatusLabel(status)).toBe('已连接')
    expect(getRemoteImStatusLabel({ ...status, state: 'disabled' })).toBe('未连接')
    expect(getRemoteImStatusLabel({ ...status, state: 'error', detail: 'login failed' })).toBe(
      '异常'
    )
  })

  it('maps message roles to compact avatars', () => {
    const message = { role: 'remote-user' } as RemoteImMessage
    expect(getRemoteImMessageAvatar(message)).toBe('手')
    expect(getRemoteImMessageAvatar({ ...message, role: 'system' })).toBe('系')
    expect(getRemoteImMessageAvatar({ ...message, role: 'aicli' })).toBe('AI')
  })

  it('uses compact message delivery status labels', () => {
    expect(getRemoteImMessageStatusLabel(message({ status: 'sent-to-aicli' }))).toBe('✓')
    expect(getRemoteImMessageStatusLabel(message({ status: 'sent-to-im' }))).toBe('✓')
    expect(getRemoteImMessageStatusLabel(message({ status: 'sent-to-im', role: 'aicli' }))).toBe('✓')
    expect(getRemoteImMessageStatusLabel(message({ status: 'streaming' }))).toBe('回复中')
    expect(getRemoteImMessageStatusLabel(message({ status: 'received' }))).toBe('')
  })

  it('disables sending without project, text, or a connected IM runtime', () => {
    const status: RemoteImStatus = {
      projectId: 'project-1',
      state: 'connected',
      detail: null,
      updatedAt: 1
    }
    expect(isRemoteImSendDisabled({ projectId: null, sessionRunning: true, text: 'hi', status })).toBe(
      true
    )
    expect(isRemoteImSendDisabled({ projectId: 'project-1', sessionRunning: true, text: ' ', status })).toBe(
      true
    )
    expect(isRemoteImSendDisabled({ projectId: 'project-1', sessionRunning: false, text: 'hi', status })).toBe(
      false
    )
    expect(isRemoteImSendDisabled({
      projectId: 'project-1',
      sessionRunning: true,
      text: 'hi',
      status: { ...status, state: 'connecting' }
    })).toBe(
      true
    )
    expect(isRemoteImSendDisabled({
      projectId: 'project-1',
      sessionRunning: true,
      text: 'hi',
      status,
    })).toBe(false)
  })

  it('derives trusted friend contacts from friend and legacy role UserID lists', () => {
    expect(getRemoteImContacts(config)).toEqual([
      { userId: 'friend-a', relation: 'friend', isContact: true },
      { userId: 'master-a', relation: 'friend', isContact: true },
      { userId: 'slave-a', relation: 'friend', isContact: true }
    ])
  })

  it('builds conversations from configured contacts and message-only peers', () => {
    const conversations = getRemoteImConversations(config, [
      message({
        id: 2,
        fromUserId: 'message-only',
        toUserId: 'local-user',
        content: 'from history',
        createdAt: 300
      }),
      message({
        id: 3,
        direction: 'outgoing',
        fromUserId: 'local-user',
        toUserId: 'slave-a',
        content: 'task',
        createdAt: 400
      })
    ])

    expect(conversations.map((item) => item.userId)).toEqual([
      'slave-a',
      'message-only',
      'friend-a',
      'master-a'
    ])
    expect(conversations[0]).toMatchObject({
      userId: 'slave-a',
      relation: 'friend',
      lastMessagePreview: 'task'
    })
    expect(conversations[1]).toMatchObject({
      userId: 'message-only',
      relation: 'friend'
    })
  })

  it('filters visible messages by selected peer UserID', () => {
    const messages = [
      message({ id: 1, fromUserId: 'friend-a', toUserId: 'local-user', content: 'friend' }),
      message({
        id: 2,
        direction: 'outgoing',
        fromUserId: 'local-user',
        toUserId: 'slave-a',
        content: 'to slave'
      }),
      message({ id: 3, fromUserId: 'slave-a', toUserId: 'local-user', content: 'from slave' })
    ]

    expect(filterRemoteImMessagesByPeer(messages, 'local-user', 'slave-a').map((item) => item.id)).toEqual([
      2,
      3
    ])
  })

  it('derives message display UserID and relation from the actual sender', () => {
    expect(
      getRemoteImMessageDisplayMeta(
        config,
        message({ id: 1, fromUserId: 'master-a', toUserId: 'local-user' })
      )
    ).toEqual({ userId: 'master-a', relation: 'friend', isContact: true })
    expect(
      getRemoteImMessageDisplayMeta(
        config,
        message({
          id: 2,
          direction: 'outgoing',
          role: 'aicli',
          fromUserId: null,
          toUserId: 'slave-a'
        })
      )
    ).toEqual({ userId: 'local-user', relation: 'friend', isContact: true })
    expect(
      getRemoteImMessageDisplayMeta(
        config,
        message({
          id: 3,
          fromUserId: 'slave-a',
          toUserId: 'local-user'
        })
      )
    ).toEqual({ userId: 'slave-a', relation: 'friend', isContact: true })
  })

  it('adds trusted friends while keeping allowed users in sync', () => {
    const next = addRemoteImContact(config, 'friend', ' slave-b ')

    // 只有好友名单这一份：加进来的人既在界面显示，也就是被允许的人。
    expect(next.friendUserIds).toContain('slave-b')
  })

  it('removes trusted friends from every contact list and keeps allowed users in sync', () => {
    const next = removeRemoteImContact(config, ' master-a ')

    expect(next.friendUserIds).not.toContain('master-a')
  })
})

describe('contact vs stranger', () => {
  // 授权判定读的是联系人配置。界面必须说同一套话，否则会出现
  // 「好友」标签和「已拒绝」状态并排显示、互相打架的情况。
  const base = { ...config, friendUserIds: ['whq-iphone'] }

  it('marks a peer that is only in the message history as a stranger', () => {
    const conversations = getRemoteImConversations(base, [
      message({
        direction: 'incoming',
        fromUserId: 'mac-multi-ai-code',
        toUserId: base.desktopUserId
      })
    ])
    const stranger = conversations.find((c) => c.userId === 'mac-multi-ai-code')

    expect(stranger).toBeDefined()
    // 出现在会话列表里可以，但不能冒充联系人。
    expect(stranger?.isContact).toBe(false)
  })

  it('marks a peer from the contact config as a contact', () => {
    const conversations = getRemoteImConversations(base, [])
    expect(conversations.find((c) => c.userId === 'whq-iphone')?.isContact).toBe(true)
  })

  it('reports isContact straight from the config', () => {
    expect(isRemoteImContact(base, 'whq-iphone')).toBe(true)
    expect(isRemoteImContact(base, 'mac-multi-ai-code')).toBe(false)
    expect(isRemoteImContact(base, '')).toBe(false)
    // 自己永远算联系人，否则自己发的消息会被标成陌生人。
    expect(isRemoteImContact(base, base.desktopUserId)).toBe(true)
  })
})
