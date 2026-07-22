import { mkdtemp, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import { DEFAULT_REMOTE_IM_CONFIG } from '../../../electron/remote-im/config.js'
import {
  DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
  hasRemoteImAccountConnectionChanged,
  mergeRemoteImAccountIntoConfig,
  normalizeRemoteImAccountConfig,
  preserveRemoteImAccountContacts,
  readRemoteImAccountConfig,
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
      allowedUserIds: ['friend-a', 'master-a', 'slave-a']
    })
  })

  it('migrates the previous built-in credential to the current credential', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1400704311,
      desktopUserId: 'mac-quark-pc',
      desktopRole: 'slave',
      userSigMode: 'secret-key',
      userSigSecretKey: '8b897045d1ee4f067a745b1b6a3fb834d1bd4c5951de43282c21b945f98ec982'
    })

    expect(account).toMatchObject({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quark-pc',
      desktopRole: 'master',
      userSigMode: 'secret-key',
      userSigSecretKey: 'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
    })
  })

  it('persists the current profile account in the Electron userData directory', async () => {
    const userDataDir = await createTempDir()
    await writeRemoteImAccountConfig(userDataDir, {
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      sdkAppId: 1400704311,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret'
    })

    await expect(readRemoteImAccountConfig(userDataDir)).resolves.toMatchObject({
      sdkAppId: 1400704311,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret'
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
        allowedUserIds: ['friend-a', 'mac-apollo-u3player', 'whq-iphone', 'slave-a']
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
      allowedUserIds: ['whq-iphone']
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
      allowedUserIds: ['whq-iphone']
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
      allowedUserIds: ['whq-iphone', 'whq-android']
    })
  })

  it('keeps existing account contacts when SDK sync returns no usable friends', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: 1600148979,
      desktopUserId: 'mac-quarkpc',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['whq-iphone']
    })

    expect(syncRemoteImAccountContactsFromSdk(account, ['', '   '])).toMatchObject({
      friendUserIds: ['whq-iphone'],
      allowedUserIds: ['whq-iphone']
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
        sdkAppId: 1400704311
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

  it('merges user account identity into project-level remote IM behavior config', () => {
    const merged = mergeRemoteImAccountIntoConfig(
      {
        ...DEFAULT_REMOTE_IM_CONFIG,
        enabled: true,
        outputFlushIntervalMs: 5000,
        outputMaxChunkChars: 900
      },
      {
        ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
        sdkAppId: 1400704311,
        desktopUserId: 'test123',
        desktopRole: 'master',
        userSigMode: 'secret-key',
        userSigSecretKey: 'secret',
        friendUserIds: ['test321'],
        allowedUserIds: ['test321']
      }
    )

    expect(merged).toMatchObject({
      enabled: true,
      sdkAppId: 1400704311,
      desktopUserId: 'test123',
      desktopRole: 'master',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['test321'],
      masterUserIds: [],
      slaveUserIds: [],
      allowedUserIds: ['test321'],
      outputFlushIntervalMs: 5000,
      outputMaxChunkChars: 900
    })
  })
})
