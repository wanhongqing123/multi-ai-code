import { mkdtemp, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import { DEFAULT_REMOTE_IM_CONFIG } from '../../../electron/remote-im/config.js'
import {
  DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
  hasRemoteImAccountConnectionChanged,
  addRemoteImAccountContact,
  mergeRemoteImAccountIntoConfig,
  normalizeRemoteImAccountConfig,
  preserveRemoteImAccountContacts,
  readRemoteImAccountConfig,
  removeRemoteImAccountContact,
  removedRemoteImAccountContactUserIds,
  syncRemoteImAccountContactsFromSdk,
  writeRemoteImAccountConfig
} from '../../../electron/remote-im/account.js'

let tempDir: string | null = null

async function createTempDir(): Promise<string> {
  tempDir = await mkdtemp(join(tmpdir(), 'remote-im-account-'))
  return tempDir
}

describe('remote IM account config', () => {
  afterEach(async () => {
    if (tempDir) await rm(tempDir, { recursive: true, force: true })
    tempDir = null
  })

  it('normalizes account identity, credentials, and legacy contacts as trusted friends', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: '1600148979',
      desktopUserId: ' test123 ',
      desktopRole: 'slave',
      userSigMode: 'secret-key',
      userSigSecretKey: ' secret ',
      friendUserIds: ['friend-a', 'friend-a', ''],
      masterUserIds: ['master-a'],
      slaveUserIds: ['slave-a']
    })

    expect(account).toEqual({
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      desktopRole: 'master',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['friend-a', 'master-a', 'slave-a'],
      masterUserIds: [],
      slaveUserIds: [],
      allowedUserIds: ['friend-a', 'master-a', 'slave-a'],
      blockedUserIds: []
    })
  })

  it('persists the current profile account in the Electron userData directory', async () => {
    const userDataDir = await createTempDir()
    await writeRemoteImAccountConfig(userDataDir, {
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      blockedUserIds: ['revoked-phone']
    })

    await expect(readRemoteImAccountConfig(userDataDir)).resolves.toMatchObject({
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      blockedUserIds: ['revoked-phone']
    })
  })

  it('does not treat contact-only account edits as connection changes', () => {
    const account = {
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quark-pc',
      desktopRole: 'master' as const,
      userSigMode: 'secret-key' as const,
      userSigSecretKey: 'secret',
      friendUserIds: ['mac-apollo-u3player'],
      allowedUserIds: ['mac-apollo-u3player']
    }

    expect(
      hasRemoteImAccountConnectionChanged(account, {
        ...account,
        friendUserIds: ['mac-apollo-u3player', 'friend-a', 'whq-iphone', 'slave-a'],
        allowedUserIds: ['friend-a', 'mac-apollo-u3player', 'whq-iphone', 'slave-a'],
        outputFlushIntervalMs: 2000,
        outputMaxChunkChars: 4000,
        remoteDesktopMode: 'disabled' as const,
        remoteDesktopControl: false
      })
    ).toBe(false)
  })

  it('preserves saved contacts when login rebinds the same account without contact fields', () => {
    const existing = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['whq-iphone'],
      allowedUserIds: ['whq-iphone'],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 4000,
      remoteDesktopMode: 'disabled' as const,
      remoteDesktopControl: false
    })

    const incoming = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret'
    })

    expect(preserveRemoteImAccountContacts(incoming, existing)).toMatchObject({
      desktopUserId: 'mac-quarkpc',
      friendUserIds: ['whq-iphone'],
      allowedUserIds: ['whq-iphone'],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 4000,
      remoteDesktopMode: 'disabled' as const,
      remoteDesktopControl: false
    })
  })

  it('uses explicitly provided contacts instead of preserving old contacts', () => {
    const existing = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['whq-iphone']
    })

    const incoming = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['whq-android']
    })

    expect(preserveRemoteImAccountContacts(incoming, existing).friendUserIds).toEqual([
      'whq-android'
    ])
  })

  it('syncs SDK friend list into account contacts and route allow-list', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['stale-friend']
    })

    expect(
      syncRemoteImAccountContactsFromSdk(account, [' whq-iphone ', 'whq-android', 'whq-iphone'])
    ).toMatchObject({
      desktopUserId: 'mac-quarkpc',
      friendUserIds: ['whq-iphone', 'whq-android'],
      masterUserIds: [],
      slaveUserIds: [],
      allowedUserIds: ['whq-iphone', 'whq-android'],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 4000,
      remoteDesktopMode: 'disabled' as const,
      remoteDesktopControl: false
    })
  })

  it('clears existing account contacts when the authoritative SDK snapshot is empty', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['whq-iphone']
    })

    expect(syncRemoteImAccountContactsFromSdk(account, ['', '   '])).toMatchObject({
      friendUserIds: [],
      allowedUserIds: [],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 4000,
      remoteDesktopMode: 'disabled' as const,
      remoteDesktopControl: false
    })
  })

  it('keeps locally revoked SDK friends blocked across reconnect snapshots', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: [],
      blockedUserIds: ['revoked-phone']
    })

    expect(
      syncRemoteImAccountContactsFromSdk(account, ['revoked-phone', 'allowed-phone'])
    ).toMatchObject({
      friendUserIds: ['allowed-phone'],
      allowedUserIds: ['allowed-phone'],
      blockedUserIds: ['revoked-phone']
    })
  })

  it('turns a locally deleted contact into a durable SDK snapshot tombstone', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      friendUserIds: ['revoked-phone', 'allowed-phone']
    })
    const deleted = removeRemoteImAccountContact(account, 'revoked-phone')

    expect(deleted).toMatchObject({
      friendUserIds: ['allowed-phone'],
      allowedUserIds: ['allowed-phone'],
      blockedUserIds: ['revoked-phone']
    })
    expect(
      syncRemoteImAccountContactsFromSdk(deleted, ['revoked-phone', 'allowed-phone'])
    ).toMatchObject({
      friendUserIds: ['allowed-phone'],
      allowedUserIds: ['allowed-phone'],
      blockedUserIds: ['revoked-phone']
    })
  })

  it('keeps output throttling and remote desktop settings on the account', () => {
    // 这四项以前存在每个项目的 project.json 里。它们描述的是「这台机器上的这个
    // 账号」怎么工作，与仓库无关——分项目存会让同一台机器出现互相矛盾的设置，
    // 比如 A 项目开着无人值守远程桌面而 B 项目关着，可屏幕只有一块。
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      outputFlushIntervalMs: 3000,
      outputMaxChunkChars: 1200,
      remoteDesktopMode: 'attended',
      remoteDesktopControl: true
    })

    expect(account).toMatchObject({
      outputFlushIntervalMs: 3000,
      outputMaxChunkChars: 1200,
      remoteDesktopMode: 'attended',
      remoteDesktopControl: true
    })
    // 合并进项目配置时必须带上，否则界面读到的永远是默认值。
    expect(mergeRemoteImAccountIntoConfig(DEFAULT_REMOTE_IM_CONFIG, account)).toMatchObject({
      outputFlushIntervalMs: 3000,
      remoteDesktopMode: 'attended',
      remoteDesktopControl: true
    })
  })

  it('falls back to safe defaults for missing or corrupt runtime settings', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      remoteDesktopMode: 'bogus',
      remoteDesktopControl: 'yes'
    })

    // 远程桌面必须收紧：配置损坏时绝不能变成「默认可被看屏幕/被操作」。
    expect(account.remoteDesktopMode).toBe('disabled')
    expect(account.remoteDesktopControl).toBe(false)
    expect(account.outputFlushIntervalMs).toBe(2000)
    expect(account.outputMaxChunkChars).toBe(4000)
  })

  it('adds a contact to the friend and allow lists', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      friendUserIds: ['existing-phone']
    })

    expect(addRemoteImAccountContact(account, ' new-phone ')).toMatchObject({
      friendUserIds: ['existing-phone', 'new-phone'],
      allowedUserIds: ['existing-phone', 'new-phone'],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 4000,
      remoteDesktopMode: 'disabled' as const,
      remoteDesktopControl: false
    })
    // 重复添加不应产生重复项。
    const twice = addRemoteImAccountContact(
      addRemoteImAccountContact(account, 'new-phone'),
      'new-phone'
    )
    expect(twice.friendUserIds.filter((id) => id === 'new-phone')).toHaveLength(1)
    // 空输入原样返回，不该凭空造出一个空 ID 的联系人。
    expect(addRemoteImAccountContact(account, '   ').friendUserIds).toEqual(['existing-phone'])
  })

  it('clears the revoke tombstone so a deleted contact can be added back', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      friendUserIds: ['revoked-phone']
    })
    const deleted = removeRemoteImAccountContact(account, 'revoked-phone')
    expect(deleted.blockedUserIds).toEqual(['revoked-phone'])

    const readded = addRemoteImAccountContact(deleted, 'revoked-phone')

    // 墓碑不清掉的话，SDK 下次同步会把这个人重新过滤掉——
    // 表现为「加了但过一会儿又没了」。
    expect(readded.blockedUserIds ?? []).not.toContain('revoked-phone')
    expect(readded.friendUserIds).toContain('revoked-phone')
    expect(
      syncRemoteImAccountContactsFromSdk(readded, ['revoked-phone'])
    ).toMatchObject({ friendUserIds: ['revoked-phone'] })
  })

  it('preserves revoke tombstones on account rebind and allows an explicit unblock', () => {
    const existing = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      friendUserIds: ['allowed-phone'],
      blockedUserIds: ['revoked-phone']
    })
    const rebindWithoutTombstones = {
      ...existing,
      blockedUserIds: undefined
    }

    expect(
      preserveRemoteImAccountContacts(rebindWithoutTombstones, existing).blockedUserIds
    ).toEqual(['revoked-phone'])
    expect(
      preserveRemoteImAccountContacts(
        { ...existing, friendUserIds: ['allowed-phone', 'revoked-phone'], blockedUserIds: [] },
        existing
      )
    ).toMatchObject({
      friendUserIds: ['allowed-phone', 'revoked-phone'],
      blockedUserIds: []
    })
  })

  it('treats login identity and credential edits as connection changes', () => {
    const account = {
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quark-pc',
      userSigMode: 'secret-key' as const,
      userSigSecretKey: 'secret'
    }

    expect(
      hasRemoteImAccountConnectionChanged(account, {
        ...account,
        // 必须与上面的 sdkAppId 不同，这条断言才有意义。用一个明显的任意值，
        // 而不是另一套真实凭证——预设只该有一套。
        sdkAppId: 1499999999
      })
    ).toBe(true)
    expect(
      hasRemoteImAccountConnectionChanged(account, {
        ...account,
        desktopUserId: 'mac-apollo-u3player'
      })
    ).toBe(true)
    expect(
      hasRemoteImAccountConnectionChanged(account, {
        ...account,
        userSigSecretKey: 'next-secret'
      })
    ).toBe(true)
  })

  it('reports every canonical trusted friend removed by a contact update', () => {
    const previous = normalizeRemoteImAccountConfig({
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      friendUserIds: ['phone-a', 'phone-b'],
      masterUserIds: ['legacy-master']
    })
    const next = normalizeRemoteImAccountConfig({
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      friendUserIds: ['phone-b']
    })

    expect(removedRemoteImAccountContactUserIds(previous, next)).toEqual([
      'phone-a',
      'legacy-master'
    ])
    expect(removedRemoteImAccountContactUserIds(next, previous)).toEqual([])
  })

  it('merges the account config into the shape the UI reads', () => {
    // 输出节流与远程桌面授权现在也归账号：合并结果必须以账号为准，
    // 传进来的项目侧值（历史残留）不能反过来盖掉它。
    const merged = mergeRemoteImAccountIntoConfig(
      {
        ...DEFAULT_REMOTE_IM_CONFIG,
        outputFlushIntervalMs: 5000,
        outputMaxChunkChars: 900
      },
      {
        ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
        sdkAppId: 1600148979,
        desktopUserId: 'test123',
        desktopRole: 'master',
        userSigMode: 'secret-key',
        userSigSecretKey: 'secret',
        friendUserIds: ['test321'],
        allowedUserIds: ['test321'],
        outputFlushIntervalMs: 3000,
        outputMaxChunkChars: 1200
      }
    )

    expect(merged).toMatchObject({
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      desktopRole: 'master',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['test321'],
      masterUserIds: [],
      slaveUserIds: [],
      allowedUserIds: ['test321'],
      outputFlushIntervalMs: 3000,
      outputMaxChunkChars: 1200
    })
  })
})
