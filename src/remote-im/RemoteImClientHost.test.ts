import { describe, expect, it } from 'vitest'
import type { RemoteImConfig } from '../../electron/preload.js'
import {
  getRemoteImConnectionKey,
  shouldConnectRemoteImClient
} from './RemoteImClientHost.js'

const config: RemoteImConfig = {
  enabled: true,
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
  allowedUserIds: ['test321'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

describe('RemoteImClientHost', () => {
  it('does not connect on startup before the user manually logs in', () => {
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config,
        loginRequested: false
      })
    ).toBe(false)
  })

  it('connects only after manual login when the current project enables remote IM', () => {
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config,
        loginRequested: true
      })
    ).toBe(true)
    expect(
      shouldConnectRemoteImClient({
        projectId: null,
        config,
        loginRequested: true
      })
    ).toBe(false)
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config: { ...config, enabled: false },
        loginRequested: true
      })
    ).toBe(false)
  })

  it('does not connect with incomplete account credentials', () => {
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config: { ...config, sdkAppId: null },
        loginRequested: true
      })
    ).toBe(false)
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config: { ...config, desktopUserId: '' },
        loginRequested: true
      })
    ).toBe(false)
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config: { ...config, userSigSecretKey: '' },
        loginRequested: true
      })
    ).toBe(false)
    expect(
      shouldConnectRemoteImClient({
        projectId: 'project-1',
        config: {
          ...config,
          userSigMode: 'endpoint',
          userSigSecretKey: '',
          userSigEndpoint: ''
        },
        loginRequested: true
      })
    ).toBe(false)
  })

  it('does not change the SDK connection key for contact or role-list edits', () => {
    const key = getRemoteImConnectionKey({
      projectId: 'project-1',
      config,
      loginRequested: true
    })

    expect(
      getRemoteImConnectionKey({
        projectId: 'project-1',
        config: {
          ...config,
          desktopRole: 'slave',
          friendUserIds: ['friend-a'],
          masterUserIds: ['master-a'],
          slaveUserIds: ['slave-a'],
          allowedUserIds: ['friend-a', 'master-a', 'slave-a'],
          outputFlushIntervalMs: 5000,
          outputMaxChunkChars: 3000
        },
        loginRequested: true
      })
    ).toBe(key)
  })

  it('changes the SDK connection key when login identity or credentials change', () => {
    const key = getRemoteImConnectionKey({
      projectId: 'project-1',
      config,
      loginRequested: true
    })

    expect(
      getRemoteImConnectionKey({
        projectId: 'project-1',
        config: { ...config, desktopUserId: 'another-user' },
        loginRequested: true
      })
    ).not.toBe(key)
    expect(
      getRemoteImConnectionKey({
        projectId: 'project-1',
        config: { ...config, userSigSecretKey: 'another-secret' },
        loginRequested: true
      })
    ).not.toBe(key)
  })
})
