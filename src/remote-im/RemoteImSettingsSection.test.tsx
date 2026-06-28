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
  it('renders only project-level remote IM behavior settings', () => {
    const html = renderToStaticMarkup(
      <RemoteImSettingsSection config={config} onChange={vi.fn()} disabled={false} />
    )

    expect(html).toContain('启用远程 IM')
    expect(html).toContain('输出刷新间隔')
    expect(html).toContain('单次回传字符数')
    expect(html).not.toContain('SDKAppID')
    expect(html).not.toContain('SecretKey')
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
