import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig, RemoteImMessage, RemoteImStatus } from '../../../electron/preload.js'
import RemoteImDrawer, { type RemoteImDrawerProps } from '../../../src/remote-im/RemoteImDrawer.js'

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
    kind: 'text',
    attachment: null,
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
    kind: 'text',
    attachment: null,
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
    kind: 'text',
    attachment: null,
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
      onSendImage={vi.fn()}
      onAddContact={vi.fn()}
      onDeleteContact={vi.fn()}
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
    expect(html).not.toContain('data-relation="master"')
    expect(html).not.toContain('data-relation="slave"')
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

  it('uses the current account as the left sidebar heading', () => {
    const html = renderDrawer()

    expect(html).not.toContain('remote-im-title')
    expect(html).not.toContain('<strong>会话</strong>')
    expect(html).not.toContain('remote-im-account-label')
    expect(html).toContain('<div class="remote-im-sidebar-head"><span>desktop_bot</span></div>')
  })

  it('renders selected peer messages as markdown', () => {
    const html = renderDrawer()

    expect(html).toContain('<strong>Task</strong>')
    expect(html).toContain('<li><code>npm test</code> passed</li>')
  })

  it('renders an image picker button in the composer', () => {
    const html = renderDrawer()

    expect(html).toContain('class="remote-im-image-button"')
    expect(html).toContain('aria-label="发送图片"')
    expect(html).toContain('type="file"')
    expect(html).toContain('accept="image/jpeg,image/png,image/gif,image/webp"')
  })

  it('renders image messages as image previews', () => {
    const html = renderDrawer({
      messages: [
        {
          ...messages[1],
          id: 20,
          content: '[图片消息] photo.png',
          kind: 'image',
          attachment: {
            type: 'image',
            localPath: '/tmp/photo.png',
            remoteUrl: null,
            thumbnailUrl: null,
            width: 640,
            height: 480,
            sizeBytes: 4096,
            fileName: 'photo.png',
            mimeType: 'image/png',
            sdkImageId: null
          }
        }
      ]
    })

    expect(html).toContain('class="remote-im-image-preview"')
    expect(html).toContain('src="file:///tmp/photo.png"')
    expect(html).toContain('photo.png')
  })

  it('shows sent message status as a compact check mark', () => {
    const html = renderDrawer()

    expect(html).toContain('class="remote-im-message-status" title="已发送">✓</span>')
    expect(html).not.toContain('class="remote-im-message-status">已发送</span>')
  })

  it('renders GFM markdown tables in AICLI messages', () => {
    const html = renderDrawer({
      messages: [
        {
          ...messages[1],
          role: 'aicli',
          content: [
            '## 目录结构',
            '| 目录 | 作用 |',
            '|------|------|',
            '| `chrome/` | 浏览器主体 |',
            '| `content/` | 核心渲染引擎 |'
          ].join('\n')
        }
      ]
    })

    expect(html).toContain('<table>')
    expect(html).toContain('<th>目录</th>')
    expect(html).toContain('<td><code>chrome/</code></td>')
    expect(html).toContain('<td>浏览器主体</td>')
  })

  it('renders message UserID and trusted-friend labels instead of role avatars', () => {
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
    expect(html).toContain('data-message-relation="friend"')
    expect(html).not.toContain('data-message-relation="master"')
    expect(html).not.toContain('data-message-relation="slave"')
    expect(html).not.toContain('remote-im-avatar')
    expect(html).not.toContain('AICLI 输出')
  })

  it('renders an add-contact form with only relation and UserID fields', () => {
    const html = renderDrawer()

    expect(html).toContain('remote-im-add-contact')
    expect(html).toContain('name="userId"')
    expect(html).not.toContain('name="relation"')
    expect(html).not.toContain('option value="master"')
    expect(html).not.toContain('option value="slave"')
    expect(html).not.toContain('name="sdkAppId"')
    expect(html).not.toContain('name="secretKey"')
  })

  it('renders a delete action for each trusted contact conversation', () => {
    const html = renderDrawer()

    expect(html).toContain('class="remote-im-delete-contact"')
    expect(html).toContain('aria-label="删除好友 desktop_slave 及聊天历史"')
    expect(html).toContain('aria-label="删除好友 friend_a 及聊天历史"')
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

  it('does not offer control-command suggestions in the Electron drawer', () => {
    // 控制命令入口只保留在远端客户端（Qt 桌面 IM / iOS / Android）；
    // Electron 抽屉发出的消息是发给手机好友的，不会进入命令解析，
    // 在这里提示 /status 等命令只会误导。
    const html = renderDrawer({ input: '/' })

    expect(html).not.toContain('remote-im-command-suggestions')
  })

  it('keeps manual sending available for legacy slave accounts', () => {
    const html = renderDrawer({
      config: { ...config, desktopRole: 'slave' },
      input: 'hello'
    })

    expect(html).not.toContain('<button type="submit" disabled=""')
    expect(html).not.toContain('奴隶模式')
  })

  it('does not expose a global message clearing action', () => {
    const html = renderDrawer()

    expect(html).not.toContain('remote-im-clear')
    expect(html).not.toContain('清空远程 IM 消息')
    expect(html).not.toContain('>Clear</button>')
  })

  it('shows neutral remote IM status details when connection fails', () => {
    const html = renderDrawer({
      status: {
        ...status,
        state: 'error',
        detail: 'Tencent IM login failed (70013): invalid usersig'
      }
    })

    expect(html).toContain('异常')
    expect(html).toContain('IM 登录失败')
    expect(html).toContain('登录凭证无效')
    expect(html).not.toContain('Tencent IM login failed')
    expect(html).not.toContain('usersig')
  })
})
