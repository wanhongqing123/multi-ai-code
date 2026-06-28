import { mkdtemp, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import { DEFAULT_REMOTE_IM_CONFIG } from './config.js'
import {
  DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
  mergeRemoteImAccountIntoConfig,
  normalizeRemoteImAccountConfig,
  readRemoteImAccountConfig,
  writeRemoteImAccountConfig
} from './account.js'

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

  it('normalizes account identity, credentials, and contacts independently from project settings', () => {
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
      desktopRole: 'slave',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      friendUserIds: ['friend-a'],
      masterUserIds: ['master-a'],
      slaveUserIds: ['slave-a'],
      allowedUserIds: ['friend-a', 'master-a', 'slave-a']
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
        slaveUserIds: ['test321'],
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
      slaveUserIds: ['test321'],
      allowedUserIds: ['test321'],
      outputFlushIntervalMs: 5000,
      outputMaxChunkChars: 900
    })
  })
})
