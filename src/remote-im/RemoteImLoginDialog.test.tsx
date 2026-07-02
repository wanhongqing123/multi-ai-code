import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type {
  RemoteImAccountConfig,
  RemoteImConfig,
  RemoteImLoginState
} from '../../electron/preload.js'
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

const projectConfig: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1600148979,
  desktopUserId: 'test123',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: ['test321'],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: ['test321'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

describe('RemoteImLoginDialog', () => {
  it('renders IM login with fixed credentials and project forwarding settings', () => {
    const html = renderToStaticMarkup(
      <RemoteImLoginDialog
        open
        loginState={loginState}
        projectConfig={projectConfig}
        projectConfigReady={true}
        saving={false}
        error={null}
        onClose={vi.fn()}
        onSubmit={vi.fn()}
      />
    )

    expect(html).toContain('IM 登录')
    expect(html).toContain('登录账号')
    expect(html).toContain('test123')
    expect(html).toContain('基础 IM 配置固定')
    expect(html).not.toContain('Profile')
    expect(html).not.toContain('切换')
    expect(html).not.toContain('角色')
    expect(html).not.toContain('主人')
    expect(html).not.toContain('奴隶')
    expect(html).not.toContain('凭证预设')
    expect(html).not.toContain('UserSig 方式')
    expect(html).not.toContain('type="password"')
    expect(html).not.toContain('填入 IM 应用 SecretKey')
    expect(html).not.toContain('测试凭证 1400704311')
    expect(html).not.toContain('SDKAppID')
    expect(html).not.toContain('SecretKey')
    expect(html).toContain('通信配置已内置')
    expect(html).toContain('连接凭证使用内置测试配置')
    expect(html).toContain('当前项目 IM 配置')
    expect(html).toContain('AI 输出回传间隔')
    expect(html).toContain('每隔这段时间合并一次 AICLI 新输出再发回 IM')
    expect(html).toContain('单次回传字符数')
  })

  it('does not render when closed', () => {
    const html = renderToStaticMarkup(
      <RemoteImLoginDialog
        open={false}
        loginState={loginState}
        projectConfig={projectConfig}
        projectConfigReady={true}
        saving={false}
        error={null}
        onClose={vi.fn()}
        onSubmit={vi.fn()}
      />
    )

    expect(html).toBe('')
  })

  it('keeps the login dialog open when clicking the backdrop', () => {
    const html = renderToStaticMarkup(
      <RemoteImLoginDialog
        open
        loginState={loginState}
        projectConfig={projectConfig}
        projectConfigReady={true}
        saving={false}
        error={null}
        onClose={vi.fn()}
        onSubmit={vi.fn()}
      />
    )

    expect(html).toContain('modal-backdrop')
    expect(html).toContain('data-close-on-backdrop="false"')
  })

  it('applies saved contacts without allowing saved credentials to override the fixed preset', () => {
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
      desktopRole: 'master',
      sdkAppId: 1600148979,
      userSigMode: 'secret-key',
      masterUserIds: ['test1234'],
      allowedUserIds: ['test1234']
    })
    expect(applyLoadedRemoteImLoginAccount(draft, { ...saved, desktopUserId: 'other' })).toBe(draft)
  })
})
