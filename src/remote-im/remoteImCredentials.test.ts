import { describe, expect, it } from 'vitest'
import type { RemoteImAccountConfig } from '../../electron/preload.js'
import {
  applyRemoteImCredentialPreset,
  getSelectedRemoteImCredentialPresetId,
  REMOTE_IM_CREDENTIAL_PRESETS
} from './remoteImCredentials.js'

const account: RemoteImAccountConfig = {
  provider: 'tencent-im',
  sdkAppId: null,
  desktopUserId: 'test123',
  desktopRole: 'master',
  userSigMode: 'endpoint',
  userSigEndpoint: '',
  userSigSecretKey: '',
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: []
}

describe('remote IM credentials', () => {
  it('keeps both built-in Tencent IM credential presets', () => {
    expect(REMOTE_IM_CREDENTIAL_PRESETS.map((item) => item.sdkAppId)).toEqual([
      1400704311,
      1600148979
    ])
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
    const next = applyRemoteImCredentialPreset(account, 'tencent-im-1400704311')

    expect(getSelectedRemoteImCredentialPresetId(next)).toBe('tencent-im-1400704311')
  })
})
