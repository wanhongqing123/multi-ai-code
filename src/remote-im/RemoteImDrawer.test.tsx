import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig, RemoteImMessage, RemoteImStatus } from '../../electron/preload.js'
import RemoteImDrawer, { type RemoteImDrawerProps } from './RemoteImDrawer.js'

const status: RemoteImStatus = {
  projectId: 'project-1',
  state: 'connected',
  detail: null,
  updatedAt: 1
}

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1400000000,
  desktopUserId: 'desktop_bot',
  desktopRole: 'master',
  userSigMode: 'endpoint',
  userSigEndpoint: 'https://example.test/sig',
  userSigSecretKey: '',
  friendUserIds: ['friend_a'],
  masterUserIds: ['phone_admin'],
  slaveUserIds: ['desktop_slave'],
  allowedUserIds: ['friend_a', 'phone_admin', 'desktop_slave'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

const messages: RemoteImMessage[] = [
  {
    id: 1,
    projectId: 'project-1',
    sessionId: 'session-main',
    provider: 'tencent-im',
    remoteMessageId: 'remote-1',
    fromUserId: 'phone_admin',
    toUserId: 'desktop_bot',
    role: 'remote-user',
    direction: 'incoming',
    content: 'check build',
    status: 'sent-to-aicli',
    error: null,
    createdAt: new Date('2026-06-23T14:18:04Z').getTime(),
    sentToAicliAt: null,
    sentToImAt: null
  },
  {
    id: 2,
    projectId: 'project-1',
    sessionId: 'session-main',
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: 'desktop_bot',
    toUserId: 'desktop_slave',
    role: 'remote-user',
    direction: 'outgoing',
    content: ['**Task**', '', '- `npm test` passed'].join('\n'),
    status: 'sent-to-im',
    error: null,
    createdAt: new Date('2026-06-23T14:18:09Z').getTime(),
    sentToAicliAt: null,
    sentToImAt: null
  },
  {
    id: 3,
    projectId: 'project-1',
    sessionId: 'session-main',
    provider: 'tencent-im',
    remoteMessageId: 'remote-3',
    fromUserId: 'friend_a',
    toUserId: 'desktop_bot',
    role: 'remote-user',
    direction: 'incoming',
    content: 'hello from friend',
    status: 'received',
    error: null,
    createdAt: new Date('2026-06-23T14:18:12Z').getTime(),
    sentToAicliAt: null,
    sentToImAt: null
  }
]

function renderDrawer(overrides: Partial<RemoteImDrawerProps> = {}): string {
  return renderToStaticMarkup(
    <RemoteImDrawer
      open
      projectId="project-1"
      sessionRunning
      status={status}
      config={config}
      messages={messages}
      selectedPeerUserId="desktop_slave"
      input=""
      onInputChange={vi.fn()}
      onSelectPeer={vi.fn()}
      onSend={vi.fn()}
      onAddContact={vi.fn()}
      onClear={vi.fn()}
      onLoginClick={vi.fn()}
      onClose={vi.fn()}
      {...overrides}
    />
  )
}

describe('RemoteImDrawer', () => {
  it('renders a two-column UserID chat surface', () => {
    const html = renderDrawer()

    expect(html).toContain('remote-im-drawer')
    expect(html).toContain('remote-im-shell')
    expect(html).toContain('remote-im-sidebar')
    expect(html).toContain('remote-im-chat')
    expect(html).toContain('remote-im-relation-tabs')
    expect(html).toContain('data-relation="recent"')
    expect(html).toContain('data-relation="friend"')
    expect(html).toContain('data-relation="master"')
    expect(html).toContain('data-relation="slave"')
    expect(html).toContain('friend_a')
    expect(html).toContain('phone_admin')
    expect(html).toContain('desktop_slave')
    expect(html).toContain('remote-im-chat-title')
    expect(html).not.toContain('SDKAppID')
    expect(html).not.toContain('SecretKey')
    expect(html).not.toContain('联系资料')
    expect(html).not.toContain('联系人资料')
  })

  it('does not render when closed', () => {
    expect(renderDrawer({ open: false })).toBe('')
  })

  it('renders a login account action in the header', () => {
    const html = renderDrawer()

    expect(html).toContain('remote-im-login-action')
    expect(html).toContain('desktop_bot')
    expect(html).toContain('登录')
    expect(html).not.toContain('切换账号')
  })

  it('prompts login when there is no UserID account', () => {
    const html = renderDrawer({
      config: {
        ...config,
        desktopUserId: '',
        sdkAppId: null
      }
    })

    expect(html).toContain('remote-im-login-action')
    expect(html).toContain('登录')
  })

  it('renders selected peer messages as markdown', () => {
    const html = renderDrawer()

    expect(html).toContain('<strong>Task</strong>')
    expect(html).toContain('<li><code>npm test</code> passed</li>')
  })

  it('renders message UserID and relation labels instead of role avatars', () => {
    const html = renderDrawer({
      selectedPeerUserId: 'desktop_slave',
      messages: [
        messages[1],
        {
          ...messages[1],
          id: 4,
          role: 'aicli',
          fromUserId: null,
          toUserId: 'desktop_slave',
          content: 'result from local AICLI'
        },
        {
          ...messages[1],
          id: 5,
          direction: 'incoming',
          fromUserId: 'desktop_slave',
          toUserId: 'desktop_bot',
          content: 'slave reply'
        }
      ]
    })

    expect(html).toContain('desktop_bot')
    expect(html).toContain('desktop_slave')
    expect(html).toContain('data-message-relation="master"')
    expect(html).toContain('data-message-relation="slave"')
    expect(html).not.toContain('remote-im-avatar')
    expect(html).not.toContain('AICLI 输出')
  })

  it('renders an add-contact form with only relation and UserID fields', () => {
    const html = renderDrawer()

    expect(html).toContain('remote-im-add-contact')
    expect(html).toContain('name="relation"')
    expect(html).toContain('name="userId"')
    expect(html).toContain('option value="friend"')
    expect(html).toContain('option value="master"')
    expect(html).toContain('option value="slave"')
    expect(html).not.toContain('name="sdkAppId"')
    expect(html).not.toContain('name="secretKey"')
  })

  it('keeps peer IM sending available without a running AICLI session', () => {
    const html = renderDrawer({ sessionRunning: false, input: 'hello' })

    expect(html).not.toContain('<button type="submit" disabled=""')
  })

  it('hides low-value automatic acknowledgements and received status labels', () => {
    const html = renderDrawer({
      selectedPeerUserId: 'phone_admin',
      messages: [
        {
          ...messages[0],
          id: 4,
          role: 'remote-user',
          direction: 'incoming',
          fromUserId: 'phone_admin',
          content: '已发送给当前 AICLI，开始处理。',
          status: 'received'
        },
        {
          ...messages[0],
          id: 5,
          role: 'aicli',
          direction: 'outgoing',
          fromUserId: 'desktop_bot',
          toUserId: 'phone_admin',
          content: '处理完成',
          status: 'received'
        }
      ]
    })

    expect(html).not.toContain('已发送给当前 AICLI，开始处理。')
    expect(html).toContain('处理完成')
    expect(html).not.toContain('已接收')
  })

  it('disables send when IM is not connected or no peer is selected', () => {
    const connectingHtml = renderDrawer({
      status: { ...status, state: 'connecting' },
      input: 'hello'
    })
    const noPeerHtml = renderDrawer({
      selectedPeerUserId: null,
      config: {
        ...config,
        friendUserIds: [],
        masterUserIds: [],
        slaveUserIds: [],
        allowedUserIds: []
      },
      messages: [],
      input: 'hello'
    })

    expect(connectingHtml).toContain('<button type="submit" disabled=""')
    expect(noPeerHtml).toContain('<button type="submit" disabled=""')
  })

  it('disables manual sending when this desktop is a slave', () => {
    const html = renderDrawer({
      config: { ...config, desktopRole: 'slave' },
      input: 'hello'
    })

    expect(html).toContain('<button type="submit" disabled=""')
  })

  it('disables clearing when there is no project or no remote IM message', () => {
    const emptyHtml = renderDrawer({ messages: [] })
    const noProjectHtml = renderDrawer({ projectId: null })

    expect(emptyHtml).toContain('class="remote-im-clear"')
    expect(emptyHtml).toContain('disabled="">Clear')
    expect(noProjectHtml).toContain('class="remote-im-clear"')
    expect(noProjectHtml).toContain('disabled="">Clear')
  })

  it('shows remote IM status details when connection fails', () => {
    const html = renderDrawer({
      status: {
        ...status,
        state: 'error',
        detail: 'Tencent IM login failed (70013): invalid usersig'
      }
    })

    expect(html).toContain('异常')
    expect(html).toContain('Tencent IM login failed')
  })
})
