import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { RemoteImAccountConfig, RemoteImLoginState } from '../../electron/preload.js'
import RemoteImLoginDialog, { applyLoadedRemoteImLoginAccount } from './RemoteImLoginDialog.js'

const account: RemoteImAccountConfig = {
  provider: 'tencent-im',
  sdkAppId: 1400704311,
  desktopUserId: 'test123',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: ['test321'],
  allowedUserIds: ['test321']
}

const loginState: RemoteImLoginState = {
  profileId: 'test123',
  account
}

describe('RemoteImLoginDialog', () => {
  it('renders plain IM login fields without profile switching', () => {
    const html = renderToStaticMarkup(
      <RemoteImLoginDialog
        open
        loginState={loginState}
        saving={false}
        error={null}
        onClose={vi.fn()}
        onSubmit={vi.fn()}
      />
    )

    expect(html).toContain('IM 登录')
    expect(html).toContain('UserID')
    expect(html).toContain('test123')
    expect(html).not.toContain('Profile')
    expect(html).not.toContain('切换')
    expect(html).toContain('主人')
    expect(html).toContain('测试凭证 1400704311')
    expect(html).toContain('测试凭证 1600148979')
    expect(html).not.toContain('输出刷新间隔')
    expect(html).not.toContain('单次回传字符数')
  })

  it('does not render when closed', () => {
    const html = renderToStaticMarkup(
      <RemoteImLoginDialog
        open={false}
        loginState={loginState}
        saving={false}
        error={null}
        onClose={vi.fn()}
        onSubmit={vi.fn()}
      />
    )

    expect(html).toBe('')
  })

  it('applies a saved account when it belongs to the typed UserID', () => {
    const draft = {
      ...account,
      desktopUserId: 'test12345',
      desktopRole: 'master',
      masterUserIds: [],
      slaveUserIds: [],
      allowedUserIds: []
    } satisfies RemoteImAccountConfig
    const saved = {
      ...account,
      desktopUserId: 'test12345',
      desktopRole: 'slave',
      masterUserIds: ['test1234'],
      slaveUserIds: [],
      allowedUserIds: ['test1234']
    } satisfies RemoteImAccountConfig

    expect(applyLoadedRemoteImLoginAccount(draft, saved)).toMatchObject({
      desktopUserId: 'test12345',
      desktopRole: 'slave',
      masterUserIds: ['test1234'],
      allowedUserIds: ['test1234']
    })
    expect(applyLoadedRemoteImLoginAccount(draft, { ...saved, desktopUserId: 'other' })).toBe(draft)
  })
})
