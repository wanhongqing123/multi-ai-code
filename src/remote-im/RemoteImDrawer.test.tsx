import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { RemoteImMessage, RemoteImStatus } from '../../electron/preload.js'
import RemoteImDrawer from './RemoteImDrawer.js'

const status: RemoteImStatus = {
  projectId: 'project-1',
  state: 'connected',
  detail: null,
  updatedAt: 1
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
    content: '检查构建',
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
    fromUserId: null,
    toUserId: 'phone_admin',
    role: 'aicli',
    direction: 'outgoing',
    content: '我会先检查 package.json。',
    status: 'streaming',
    error: null,
    createdAt: new Date('2026-06-23T14:18:09Z').getTime(),
    sentToAicliAt: null,
    sentToImAt: null
  }
]

describe('RemoteImDrawer', () => {
  it('renders a minimal conversation drawer', () => {
    const html = renderToStaticMarkup(
      <RemoteImDrawer
        open
        projectId="project-1"
        sessionRunning
        status={status}
        messages={messages}
        input=""
        onInputChange={vi.fn()}
        onSend={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(html).toContain('remote-im-drawer')
    expect(html).toContain('远程 IM')
    expect(html).toContain('已连接')
    expect(html).toContain('检查构建')
    expect(html).toContain('我会先检查 package.json。')
    expect(html).not.toContain('当前链路')
    expect(html).not.toContain('策略')
  })

  it('does not render when closed', () => {
    const html = renderToStaticMarkup(
      <RemoteImDrawer
        open={false}
        projectId="project-1"
        sessionRunning
        status={status}
        messages={messages}
        input=""
        onInputChange={vi.fn()}
        onSend={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(html).toBe('')
  })

  it('disables send when there is no running session', () => {
    const html = renderToStaticMarkup(
      <RemoteImDrawer
        open
        projectId="project-1"
        sessionRunning={false}
        status={status}
        messages={[]}
        input="hello"
        onInputChange={vi.fn()}
        onSend={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(html).toContain('disabled=""')
  })
})
