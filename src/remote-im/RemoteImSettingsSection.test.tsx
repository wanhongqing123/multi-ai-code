import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig } from '../../electron/preload.js'
import RemoteImSettingsSection from './RemoteImSettingsSection.js'

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

describe('RemoteImSettingsSection', () => {
  it('renders a read-only remote IM settings summary', () => {
    const html = renderToStaticMarkup(
      <RemoteImSettingsSection config={config} onChange={vi.fn()} disabled={false} />
    )

    expect(html).toContain('当前状态')
    expect(html).toContain('已开启')
    expect(html).toContain('远程 IM 账号、SDKAppID、SecretKey 和连接动作由登录入口管理')
    expect(html).not.toContain('type="checkbox"')
    expect(html).not.toContain('<input')
    expect(html).not.toContain('<select')
    expect(html).not.toContain('desktop_bot')
    expect(html).not.toContain('https://example.test/sig')
    expect(html).not.toContain('1400704311')
    expect(html).not.toContain('1600148979')
  })

  it('keeps contact management out of the settings section', () => {
    const html = renderToStaticMarkup(
      <RemoteImSettingsSection config={config} onChange={vi.fn()} disabled={false} />
    )

    expect(html).not.toContain('friend_a')
    expect(html).not.toContain('phone_admin')
    expect(html).not.toContain('desktop_slave')
  })
})
