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
  readRemoteImAccountConfig,
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

  it('normalizes account identity and credentials', () => {
    const account = normalizeRemoteImAccountConfig({
      sdkAppId: '1600148979',
      desktopUserId: ' test123 ',
      userSigMode: 'secret-key',
      userSigSecretKey: ' secret ',
    })

    expect(account).toEqual({
      ...DEFAULT_REMOTE_IM_ACCOUNT_CONFIG,
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
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
    })

    await expect(readRemoteImAccountConfig(userDataDir)).resolves.toMatchObject({
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
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
        userSigMode: 'secret-key',
        userSigSecretKey: 'secret',
        outputFlushIntervalMs: 3000,
        outputMaxChunkChars: 1200
      }
    )

    expect(merged).toMatchObject({
      sdkAppId: 1600148979,
      desktopUserId: 'test123',
      userSigMode: 'secret-key',
      userSigSecretKey: 'secret',
      outputFlushIntervalMs: 3000,
      outputMaxChunkChars: 1200
    })
  })
})
