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
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1400704311,
  desktopUserId: 'desktop-a',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'local-secret',
  friendUserIds: ['friend-b'],
  allowedUserIds: ['friend-b', 'master-b', 'slave-b'],
  masterUserIds: ['master-b'],
  slaveUserIds: ['slave-b'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

const slaveConfig: RemoteImConfig = {
  ...masterConfig,
  desktopUserId: 'desktop-b',
  desktopRole: 'slave',
  friendUserIds: ['friend-c'],
  allowedUserIds: ['friend-c', 'master-a', 'slave-c'],
  masterUserIds: ['master-a'],
  slaveUserIds: ['slave-c']
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

  it('chooses configured trusted friends before legacy role entries for default manual sends', () => {
    expect(resolveDefaultRemoteImPeerUserId(masterConfig)).toBe('friend-b')
    expect(resolveDefaultRemoteImPeerUserId({ ...masterConfig, friendUserIds: [] })).toBe('master-b')
    expect(resolveDefaultRemoteImPeerUserId({
      ...masterConfig,
      friendUserIds: [],
      masterUserIds: []
    })).toBe('slave-b')
    expect(resolveDefaultRemoteImPeerUserId(slaveConfig)).toBe('friend-c')
  })
})
