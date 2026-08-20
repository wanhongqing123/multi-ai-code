import { describe, expect, it } from 'vitest'
import type { RemoteImConfig } from '../../../electron/remote-im/types.js'
import {
  canManuallySendToRemoteImPeer,
  canRouteRemoteImTaskFrom,
  getRemoteImPeerRelation,
  getRemoteImPeerRole,
  resolveDefaultRemoteImPeerUserId
} from '../../../electron/remote-im/rolePermissions.js'

const masterConfig: RemoteImConfig = {
  provider: 'tencent-im',
  sdkAppId: 1600148979,
  desktopUserId: 'desktop-a',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'local-secret',
  friendUserIds: ['friend-b'],
  allowedUserIds: ['friend-b', 'master-b', 'slave-b'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200,
  remoteDesktopMode: 'disabled' as const,
  remoteDesktopControl: false
}

const slaveConfig: RemoteImConfig = {
  ...masterConfig,
  desktopUserId: 'desktop-b',
  friendUserIds: ['friend-c'],
  allowedUserIds: ['friend-c', 'master-a', 'slave-c'],
}

describe('remote IM trusted-contact permissions', () => {
  it('resolves legacy role lists as trusted friends', () => {
    expect(getRemoteImPeerRelation(masterConfig, 'friend-b')).toBe('friend')
    expect(getRemoteImPeerRelation(masterConfig, 'master-b')).toBe('friend')
    expect(getRemoteImPeerRelation(masterConfig, 'slave-b')).toBe('friend')
    expect(getRemoteImPeerRelation(masterConfig, 'unknown')).toBeNull()
  })

  it('does not expose peer master/slave roles after trusted-contact migration', () => {
    expect(getRemoteImPeerRole(masterConfig, 'master-b')).toBeNull()
    expect(getRemoteImPeerRole(masterConfig, 'slave-b')).toBeNull()
    expect(getRemoteImPeerRole(masterConfig, 'friend-b')).toBeNull()
    expect(getRemoteImPeerRole(masterConfig, 'unknown')).toBeNull()
  })

  it('allows task routing from any trusted friend, including legacy role entries', () => {
    expect(canRouteRemoteImTaskFrom(masterConfig, 'friend-b')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(masterConfig, 'master-b')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(masterConfig, 'slave-b')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(masterConfig, 'unknown')).toMatchObject({
      ok: false,
      reason: 'sender-not-allowed'
    })
  })

  it('treats legacy local slave configs as regular accounts for inbound routing', () => {
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'friend-c')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'master-a')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'slave-c')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'unknown')).toMatchObject({
      ok: false,
      reason: 'sender-not-allowed'
    })
  })

  it('allows manual outbound messages from any local account to trusted friends', () => {
    expect(canManuallySendToRemoteImPeer(masterConfig, 'friend-b')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(masterConfig, 'master-b')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(masterConfig, 'slave-b')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(slaveConfig, 'master-a')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(slaveConfig, 'unknown')).toMatchObject({
      ok: false,
      reason: 'peer-not-allowed'
    })
  })

  it('picks the first trusted friend as the default manual send peer', () => {
    expect(resolveDefaultRemoteImPeerUserId(masterConfig)).toBe('friend-b')
    expect(resolveDefaultRemoteImPeerUserId(slaveConfig)).toBe('friend-c')
    // friendUserIds 为空时回退到 allowedUserIds——两者正常情况下同步，
    // 但配置损坏时不该直接变成「没有默认对端」。
    expect(resolveDefaultRemoteImPeerUserId({ ...masterConfig, friendUserIds: [] })).toBe('friend-b')
    expect(
      resolveDefaultRemoteImPeerUserId({ ...masterConfig, friendUserIds: [], allowedUserIds: [] })
    ).toBeNull()
  })
})
