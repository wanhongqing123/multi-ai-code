import { describe, expect, it } from 'vitest'
import type { RemoteImConfig } from './types.js'
import {
  canManuallySendToRemoteImPeer,
  canRouteRemoteImTaskFrom,
  getRemoteImPeerRelation,
  getRemoteImPeerRole,
  resolveDefaultRemoteImPeerUserId
} from './rolePermissions.js'

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

describe('remote IM role permissions', () => {
  it('resolves configured peer relations', () => {
    expect(getRemoteImPeerRelation(masterConfig, 'friend-b')).toBe('friend')
    expect(getRemoteImPeerRelation(masterConfig, 'master-b')).toBe('master')
    expect(getRemoteImPeerRelation(masterConfig, 'slave-b')).toBe('slave')
    expect(getRemoteImPeerRelation(masterConfig, 'unknown')).toBeNull()
  })

  it('resolves configured peer roles', () => {
    expect(getRemoteImPeerRole(masterConfig, 'master-b')).toBe('master')
    expect(getRemoteImPeerRole(masterConfig, 'slave-b')).toBe('slave')
    expect(getRemoteImPeerRole(masterConfig, 'friend-b')).toBeNull()
    expect(getRemoteImPeerRole(masterConfig, 'unknown')).toBeNull()
  })

  it('allows task routing only from master peers', () => {
    expect(canRouteRemoteImTaskFrom(masterConfig, 'master-b')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(masterConfig, 'slave-b')).toMatchObject({
      ok: false,
      reason: 'slave-cannot-initiate'
    })
  })

  it('allows slaves to route tasks only from master peers', () => {
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'master-a')).toMatchObject({ ok: true })
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'slave-c')).toMatchObject({
      ok: false,
      reason: 'slave-to-slave-blocked'
    })
    expect(canRouteRemoteImTaskFrom(slaveConfig, 'unknown')).toMatchObject({
      ok: false,
      reason: 'sender-not-allowed'
    })
  })

  it('allows manual outbound messages only from masters', () => {
    expect(canManuallySendToRemoteImPeer(masterConfig, 'friend-b')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(masterConfig, 'master-b')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(masterConfig, 'slave-b')).toMatchObject({ ok: true })
    expect(canManuallySendToRemoteImPeer(slaveConfig, 'master-a')).toMatchObject({
      ok: false,
      reason: 'slave-cannot-initiate'
    })
  })

  it('chooses a master peer before a slave peer for default manual sends', () => {
    expect(resolveDefaultRemoteImPeerUserId(masterConfig)).toBe('master-b')
    expect(resolveDefaultRemoteImPeerUserId({ ...masterConfig, masterUserIds: [] })).toBe('slave-b')
    expect(resolveDefaultRemoteImPeerUserId(slaveConfig)).toBeNull()
  })
})
