import { describe, expect, it } from 'vitest'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  toRemoteImProjectConfig,
  toRemoteImProjectMetaConfig,
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

  it('allows project configs without account fields because login is user-level', () => {
    const result = validateRemoteImConfig({ ...DEFAULT_REMOTE_IM_CONFIG })

    expect(result.ok).toBe(true)
  })

  it('allows test configs without contacts so contacts can be added from the IM panel', () => {
    const result = validateRemoteImConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      sdkAppId: 1600148979,
      desktopUserId: 'desktop_bot',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret-for-local-test'
    })

    expect(result.ok).toBe(true)
  })

  it('normalizes legacy slave configs as regular desktop accounts', () => {
    const config = normalizeRemoteImConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'desktop_bot',
      desktopRole: 'slave',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret-for-local-test'
    })

    expect(config.desktopRole).toBe('master')
    expect(validateRemoteImConfig(config).ok).toBe(true)
  })
})

describe('remote desktop mode in remote IM config', () => {
  it('defaults to disabled so a new install is never viewable', () => {
    // 这台机器上跑着 AICLI 和用户的仓库：屏幕共享必须显式开一次，
    // 不能因为装了新版本就默认可被查看。
    expect(DEFAULT_REMOTE_IM_CONFIG.remoteDesktopMode).toBe('disabled')
    expect(normalizeRemoteImConfig({}).remoteDesktopMode).toBe('disabled')
  })

  it('falls back to disabled for unknown or corrupted values', () => {
    // 配置损坏时必须收紧而不是放宽。
    for (const value of ['viewer', '', null, 42, {}, 'UNATTENDED']) {
      expect(normalizeRemoteImConfig({ remoteDesktopMode: value }).remoteDesktopMode, String(value)).toBe(
        'disabled'
      )
    }
  })

  it('keeps the three supported modes', () => {
    for (const mode of ['disabled', 'attended', 'unattended'] as const) {
      expect(normalizeRemoteImConfig({ remoteDesktopMode: mode }).remoteDesktopMode).toBe(mode)
    }
  })

  it('carries the mode through project config conversion', () => {
    // toRemoteImProjectConfig 以 DEFAULT 为底，漏传就会在保存时把用户的选择重置。
    const config = { ...DEFAULT_REMOTE_IM_CONFIG, remoteDesktopMode: 'unattended' as const }
    expect(toRemoteImProjectConfig(config).remoteDesktopMode).toBe('unattended')
  })

  it('persists only the four project-owned fields into project.json', () => {
    // 以前整份 RemoteImConfig 都写进 project.json，于是每个项目里都躺着一份
    // sdkAppId/desktopUserId/friendUserIds 的空壳——读取时全被账号库覆盖，
    // 纯冗余，还让人误以为这些能按项目改。
    const meta = toRemoteImProjectMetaConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      sdkAppId: 1400000000,
      desktopUserId: 'someone-else',
      userSigSecretKey: 'super-secret',
      friendUserIds: ['phone-user'],
      allowedUserIds: ['phone-user'],
      outputFlushIntervalMs: 3000,
      outputMaxChunkChars: 1200,
      remoteDesktopMode: 'attended',
      remoteDesktopControl: true
    } as never)

    expect(Object.keys(meta).sort()).toEqual([
      'outputFlushIntervalMs',
      'outputMaxChunkChars',
      'remoteDesktopControl',
      'remoteDesktopMode'
    ])
    expect(meta).toEqual({
      outputFlushIntervalMs: 3000,
      outputMaxChunkChars: 1200,
      remoteDesktopMode: 'attended',
      remoteDesktopControl: true
    })
    // 凭证绝不能进项目文件：项目目录可能被同步/分享，账号库才是它们的家。
    expect(JSON.stringify(meta)).not.toContain('super-secret')
    expect(JSON.stringify(meta)).not.toContain('someone-else')
  })

  it('strips every connection-relevant field from the project config', () => {
    // setRemoteImConfig 据此认定「写项目配置改变不了连接」，所以保存时不再广播
    // disconnected。要是哪天这里开始保留某个连接字段，那个假设就不成立了：
    // 保存可能真的需要重连，而状态却没人重置——先在这里炸，比线上徽标说谎好。
    const connectionFields = [
      'provider',
      'sdkAppId',
      'desktopUserId',
      'userSigMode',
      'userSigEndpoint',
      'userSigSecretKey'
    ] as const

    const converted = toRemoteImProjectConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      provider: 'tencent-im',
      sdkAppId: 1400000000,
      desktopUserId: 'someone-else',
      userSigMode: 'secret',
      userSigEndpoint: 'https://sig.example.test/sign',
      userSigSecretKey: 'super-secret'
    } as never)

    for (const field of connectionFields) {
      expect(converted[field], field).toEqual(DEFAULT_REMOTE_IM_CONFIG[field])
    }
  })
})
