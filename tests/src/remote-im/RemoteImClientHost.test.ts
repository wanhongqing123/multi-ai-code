import { describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig } from '../../../electron/preload.js'
import {
  createRemoteImFriendSnapshotSynchronizer,
  createRemoteImLifecycleQueue,
  getRemoteImConnectionKey,
  isSameRemoteImRuntimeIdentity,
  scheduleRemoteImConnect,
  shouldConnectRemoteImClient,
  syncRemoteImContactsFromRuntime
} from '../../../src/remote-im/RemoteImClientHost.js'

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
  it('accepts outgoing events only for the exact registered runtime identity', () => {
    const current = {
      connectionId: 'connection-b',
      desktopUserId: ' desktop-b ',
      sdkAppId: 1400704311
    }

    expect(
      isSameRemoteImRuntimeIdentity(
        {
          connectionId: 'connection-b',
          desktopUserId: 'desktop-b',
          sdkAppId: 1400704311
        },
        current
      )
    ).toBe(true)
    expect(
      isSameRemoteImRuntimeIdentity(
        {
          connectionId: 'connection-a',
          desktopUserId: 'desktop-b',
          sdkAppId: 1400704311
        },
        current
      )
    ).toBe(false)
  })

  it('reruns one authoritative friend snapshot after an in-flight refresh becomes dirty', async () => {
    let finishFirst: (() => void) | undefined
    let callCount = 0
    const refresh = vi.fn(async () => {
      callCount += 1
      if (callCount === 1) {
        await new Promise<void>((resolve) => {
          finishFirst = resolve
        })
      }
    })
    const sync = createRemoteImFriendSnapshotSynchronizer(refresh)

    const first = sync()
    await vi.waitFor(() => expect(finishFirst).toBeTypeOf('function'))
    const second = sync()
    const third = sync()
    expect(refresh).toHaveBeenCalledTimes(1)
    finishFirst?.()
    await Promise.all([first, second, third])

    expect(refresh).toHaveBeenCalledTimes(2)
  })

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
      runtimeIdentity: {
        connectionId: 'runtime-1',
        desktopUserId: config.desktopUserId,
        sdkAppId: config.sdkAppId
      },
      runtime: {
        listFriendUserIds: async () => [' whq-iphone ', 'whq-iphone']
      },
      syncContacts,
      onContactsSynced
    })

    expect(syncContacts).toHaveBeenCalledWith('project-1', ['whq-iphone'], {
      connectionId: 'runtime-1',
      desktopUserId: config.desktopUserId,
      sdkAppId: config.sdkAppId
    })
    expect(onContactsSynced).toHaveBeenCalledWith({
      config: nextConfig,
      loginState: nextLoginState
    })
  })

  it('forwards a successful empty runtime friend snapshot to clear account authority', async () => {
    const syncContacts = vi.fn(async () => ({
      ok: true as const,
      value: { ...config, friendUserIds: [], allowedUserIds: [] },
      loginState: {
        profileId: 'mac-quarkpc',
        account: {
          provider: config.provider,
          sdkAppId: config.sdkAppId,
          desktopUserId: config.desktopUserId,
          desktopRole: config.desktopRole,
          userSigMode: config.userSigMode,
          userSigEndpoint: config.userSigEndpoint,
          userSigSecretKey: config.userSigSecretKey,
          friendUserIds: [],
          masterUserIds: [],
          slaveUserIds: [],
          allowedUserIds: []
        }
      }
    }))

    await syncRemoteImContactsFromRuntime({
      projectId: 'project-1',
      runtimeIdentity: {
        connectionId: 'runtime-empty',
        desktopUserId: config.desktopUserId,
        sdkAppId: config.sdkAppId
      },
      runtime: { listFriendUserIds: async () => [] },
      syncContacts
    })

    expect(syncContacts).toHaveBeenCalledWith('project-1', [], {
      connectionId: 'runtime-empty',
      desktopUserId: config.desktopUserId,
      sdkAppId: config.sdkAppId
    })
  })

  it('drops a friend snapshot when its runtime becomes stale while the SDK call is pending', async () => {
    let finishSnapshot: ((userIds: string[]) => void) | undefined
    let current = true
    const syncContacts = vi.fn()
    const syncing = syncRemoteImContactsFromRuntime({
      projectId: 'project-1',
      runtimeIdentity: {
        connectionId: 'runtime-stale',
        desktopUserId: config.desktopUserId,
        sdkAppId: config.sdkAppId
      },
      runtime: {
        listFriendUserIds: () =>
          new Promise((resolve) => {
            finishSnapshot = resolve
          })
      },
      syncContacts,
      isCurrent: () => current
    })

    await vi.waitFor(() => expect(finishSnapshot).toBeTypeOf('function'))
    current = false
    finishSnapshot?.(['old-friend'])
    await syncing

    expect(syncContacts).not.toHaveBeenCalled()
  })
})
