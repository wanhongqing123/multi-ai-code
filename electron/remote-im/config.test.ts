import { describe, expect, it } from 'vitest'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  validateRemoteImConfig
} from './config.js'

describe('remote IM config', () => {
  it('normalizes missing values to a disabled Tencent IM config', () => {
    expect(normalizeRemoteImConfig(undefined)).toEqual(DEFAULT_REMOTE_IM_CONFIG)
  })

  it('trims user ids and removes empty whitelist entries', () => {
    expect(
      normalizeRemoteImConfig({
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: '1400000000',
        desktopUserId: ' desktop_bot ',
        userSigMode: 'secret-key',
        userSigEndpoint: ' https://example.test/sig ',
        userSigSecretKey: ' test_secret ',
        allowedUserIds: [' phone_admin ', '', 'phone_admin'],
        outputFlushIntervalMs: 500,
        outputMaxChunkChars: 10
      })
    ).toMatchObject({
      enabled: true,
      provider: 'tencent-im',
      sdkAppId: 1400000000,
      desktopUserId: 'desktop_bot',
      userSigMode: 'secret-key',
      userSigEndpoint: 'https://example.test/sig',
      userSigSecretKey: 'test_secret',
      allowedUserIds: ['phone_admin'],
      outputFlushIntervalMs: 1000,
      outputMaxChunkChars: 200
    })
  })

  it('migrates legacy allowed users to master peers when role lists are missing', () => {
    expect(
      normalizeRemoteImConfig({
        allowedUserIds: [' master-a ', '', 'master-a']
      })
    ).toMatchObject({
      desktopRole: 'master',
      masterUserIds: ['master-a'],
      slaveUserIds: [],
      allowedUserIds: ['master-a']
    })
  })

  it('normalizes explicit friend, master, and slave contact lists', () => {
    expect(
      normalizeRemoteImConfig({
        desktopRole: 'slave',
        friendUserIds: [' friend-a ', '', 'friend-a'],
        masterUserIds: [' master-a ', 'master-a'],
        slaveUserIds: [' slave-b ', '']
      })
    ).toMatchObject({
      desktopRole: 'slave',
      friendUserIds: ['friend-a'],
      masterUserIds: ['master-a'],
      slaveUserIds: ['slave-b'],
      allowedUserIds: ['friend-a', 'master-a', 'slave-b']
    })
  })

  it('allows enabled project configs without account fields because login is user-level', () => {
    const result = validateRemoteImConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      enabled: true
    })

    expect(result.ok).toBe(true)
  })

  it('allows enabled test configs without contacts so contacts can be added from the IM panel', () => {
    const result = validateRemoteImConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      enabled: true,
      sdkAppId: 1400704311,
      desktopUserId: 'desktop_bot',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret-for-local-test'
    })

    expect(result.ok).toBe(true)
  })

  it('allows enabled slave configs without a master peer until contacts are added', () => {
    const result = validateRemoteImConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      enabled: true,
      sdkAppId: 1400704311,
      desktopUserId: 'desktop_bot',
      desktopRole: 'slave',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret-for-local-test'
    })

    expect(result.ok).toBe(true)
  })
})
