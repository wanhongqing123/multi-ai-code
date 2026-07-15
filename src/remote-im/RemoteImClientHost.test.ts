import { describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig } from '../../electron/preload.js'
import {
  createRemoteImLifecycleQueue,
  getRemoteImConnectionKey,
  scheduleRemoteImConnect,
  shouldConnectRemoteImClient,
  syncRemoteImContactsFromRuntime
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

  it('connects after login when the current project is available', () => {
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
    ).toBe(true)
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

  it('cancels a scheduled SDK connection before it starts', async () => {
    vi.useFakeTimers()
    const startConnect = vi.fn()

    const cancel = scheduleRemoteImConnect(startConnect)
    cancel()
    await vi.advanceTimersByTimeAsync(0)

    expect(startConnect).not.toHaveBeenCalled()
    vi.useRealTimers()
  })

  it('serializes SDK connect and disconnect lifecycle tasks', async () => {
    const queue = createRemoteImLifecycleQueue()
    const events: string[] = []
    let finishFirst!: () => void

    const first = queue(async () => {
      events.push('connect:start')
      await new Promise<void>((resolve) => {
        finishFirst = resolve
      })
      events.push('connect:end')
    })
    const second = queue(async () => {
      events.push('disconnect')
    })

    await Promise.resolve()
    expect(events).toEqual(['connect:start'])
    finishFirst()
    await Promise.all([first, second])

    expect(events).toEqual(['connect:start', 'connect:end', 'disconnect'])
  })

  it('syncs runtime friend list into Electron account state after SDK connect', async () => {
    const nextConfig = {
      ...config,
      friendUserIds: ['whq-iphone'],
      allowedUserIds: ['whq-iphone']
    }
    const nextLoginState = {
      profileId: 'mac-quarkpc',
      account: {
        provider: nextConfig.provider,
        sdkAppId: nextConfig.sdkAppId,
        desktopUserId: nextConfig.desktopUserId,
        desktopRole: nextConfig.desktopRole,
        userSigMode: nextConfig.userSigMode,
        userSigEndpoint: nextConfig.userSigEndpoint,
        userSigSecretKey: nextConfig.userSigSecretKey,
        friendUserIds: nextConfig.friendUserIds,
        masterUserIds: nextConfig.masterUserIds,
        slaveUserIds: nextConfig.slaveUserIds,
        allowedUserIds: nextConfig.allowedUserIds
      }
    }
    const syncContacts = vi.fn(async () => ({
      ok: true as const,
      value: nextConfig,
      loginState: nextLoginState
    }))
    const onContactsSynced = vi.fn()

    await syncRemoteImContactsFromRuntime({
      projectId: 'project-1',
      runtime: {
        listFriendUserIds: async () => [' whq-iphone ', 'whq-iphone']
      },
      syncContacts,
      onContactsSynced
    })

    expect(syncContacts).toHaveBeenCalledWith('project-1', ['whq-iphone'])
    expect(onContactsSynced).toHaveBeenCalledWith({
      config: nextConfig,
      loginState: nextLoginState
    })
  })
})
