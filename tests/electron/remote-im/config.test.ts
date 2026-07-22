import { describe, expect, it } from 'vitest'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  validateRemoteImConfig
} from '../../../electron/remote-im/config.js'

describe('remote IM config', () => {
  it('normalizes missing values to an always-on Tencent IM config', () => {
    expect(normalizeRemoteImConfig(undefined)).toEqual(DEFAULT_REMOTE_IM_CONFIG)
  })

  it('uses a larger default IM output chunk limit to avoid splitting normal long replies', () => {
    expect(DEFAULT_REMOTE_IM_CONFIG.outputMaxChunkChars).toBe(4000)
    expect(normalizeRemoteImConfig({}).outputMaxChunkChars).toBe(4000)
  })

  it('migrates the old persisted default output chunk limit to the current default', () => {
    expect(
      normalizeRemoteImConfig({
        outputMaxChunkChars: 1200
      }).outputMaxChunkChars
    ).toBe(4000)
  })

  it('trims user ids, removes empty whitelist entries, and ignores legacy enabled=false', () => {
    expect(
      normalizeRemoteImConfig({
        enabled: false,
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

  it('migrates legacy allowed users to trusted friends when role lists are missing', () => {
    expect(
      normalizeRemoteImConfig({
        allowedUserIds: [' master-a ', '', 'master-a']
      })
    ).toMatchObject({
      desktopRole: 'master',
      friendUserIds: ['master-a'],
      masterUserIds: [],
      slaveUserIds: [],
      allowedUserIds: ['master-a']
    })
  })

  it('migrates the previous built-in credential in project configs', () => {
    expect(
      normalizeRemoteImConfig({
        sdkAppId: 1400704311,
        userSigMode: 'secret-key',
        userSigSecretKey: '8b897045d1ee4f067a745b1b6a3fb834d1bd4c5951de43282c21b945f98ec982'
      })
    ).toMatchObject({
      sdkAppId: 1600148979,
      userSigMode: 'secret-key',
      userSigSecretKey: 'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
    })
  })

  it('normalizes explicit legacy role contact lists into trusted friends', () => {
    expect(
      normalizeRemoteImConfig({
        desktopRole: 'slave',
        friendUserIds: [' friend-a ', '', 'friend-a'],
        masterUserIds: [' master-a ', 'master-a'],
        slaveUserIds: [' slave-b ', '']
      })
    ).toMatchObject({
      desktopRole: 'master',
      friendUserIds: ['friend-a', 'master-a', 'slave-b'],
      masterUserIds: [],
      slaveUserIds: [],
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

  it('normalizes legacy slave configs as regular desktop accounts', () => {
    const config = normalizeRemoteImConfig({
      enabled: true,
      sdkAppId: 1400704311,
      desktopUserId: 'desktop_bot',
      desktopRole: 'slave',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret-for-local-test'
    })

    expect(config.desktopRole).toBe('master')
    expect(validateRemoteImConfig(config).ok).toBe(true)
  })
})
