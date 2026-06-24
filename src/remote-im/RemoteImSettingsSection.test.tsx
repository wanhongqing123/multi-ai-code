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
  userSigEndpoint: 'https://example.test/sig',
  allowedUserIds: ['phone_admin'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

describe('RemoteImSettingsSection', () => {
  it('renders Tencent IM config without asking for SECRETKEY', () => {
    const html = renderToStaticMarkup(
      <RemoteImSettingsSection config={config} onChange={vi.fn()} disabled={false} />
    )

    expect(html).toContain('远程 IM')
    expect(html).toContain('SDKAppID')
    expect(html).toContain('UserSig 服务地址')
    expect(html).toContain('允许控制的 UserID')
    expect(html).not.toContain('SECRETKEY')
  })
})
