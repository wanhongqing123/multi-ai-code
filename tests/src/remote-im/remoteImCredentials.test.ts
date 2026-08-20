import { describe, expect, it } from 'vitest'
import type { RemoteImAccountConfig } from '../../../electron/preload.js'
import {
  applyDefaultRemoteImCredential,
  applyRemoteImCredentialPreset,
  DEFAULT_REMOTE_IM_CREDENTIAL_PRESET,
  getSelectedRemoteImCredentialPresetId,
  REMOTE_IM_CREDENTIAL_PRESETS
} from '../../../src/remote-im/remoteImCredentials.js'

const account: RemoteImAccountConfig = {
  provider: 'tencent-im',
  sdkAppId: null,
  desktopUserId: 'test123',
  userSigMode: 'endpoint',
  userSigEndpoint: '',
  userSigSecretKey: '',
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 4000,
  remoteDesktopMode: 'disabled' as const,
  remoteDesktopControl: false
}

describe('remote IM credentials', () => {
  it('ships exactly one credential preset, matching MaiChat', () => {
    // 只能有这一套，且必须与 MaiChat 的 RemoteIMCredentialDefaults 一致。
    // 再加一套的代价不是"多个选项"而是静默失联：两端凭证不同就是两个不同的
    // 腾讯云应用，IM 和 TRTC 都碰不到一起，且没有任何报错。
    expect(REMOTE_IM_CREDENTIAL_PRESETS.map((item) => item.sdkAppId)).toEqual([1600148979])
    expect(REMOTE_IM_CREDENTIAL_PRESETS[0].userSigSecretKey).toBe(
      'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
    )
  })

  it('applies a built-in credential preset to an account config', () => {
    const preset = REMOTE_IM_CREDENTIAL_PRESETS.find((item) => item.sdkAppId === 1600148979)
    expect(preset).toBeDefined()

    const next = applyRemoteImCredentialPreset(account, preset!.id)

    expect(next.sdkAppId).toBe(1600148979)
    expect(next.userSigMode).toBe('secret-key')
    expect(next.userSigEndpoint).toBe('')
    expect(next.userSigSecretKey).toBe(
      'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
    )
    expect(next.desktopUserId).toBe(account.desktopUserId)
  })

  it('detects the selected preset from account credentials', () => {
    const next = applyRemoteImCredentialPreset(account, 'tencent-im-1600148979')

    expect(getSelectedRemoteImCredentialPresetId(next)).toBe('tencent-im-1600148979')
  })

  it('reports no selection for credentials that match no preset', () => {
    // 账号存着预设之外的凭证时必须报"没有选中"，而不是硬认成现有预设——
    // 那会让界面显示的和实际连的不是一回事。
    const custom = {
      ...account,
      sdkAppId: 1499999999,
      userSigMode: 'secret-key' as const,
      userSigSecretKey: 'not-a-preset-secret'
    }

    expect(getSelectedRemoteImCredentialPresetId(custom)).toBe('')
  })

  it('uses the current production test credential as the fixed login default', () => {
    const next = applyDefaultRemoteImCredential({
      ...account,
      sdkAppId: 1499999999,
      userSigMode: 'endpoint',
      userSigEndpoint: 'https://example.test/sig',
      userSigSecretKey: 'old-secret'
    })

    expect(DEFAULT_REMOTE_IM_CREDENTIAL_PRESET.sdkAppId).toBe(1600148979)
    expect(next.sdkAppId).toBe(1600148979)
    expect(next.userSigMode).toBe('secret-key')
    expect(next.userSigEndpoint).toBe('')
    expect(next.userSigSecretKey).toBe(DEFAULT_REMOTE_IM_CREDENTIAL_PRESET.userSigSecretKey)
  })
})
