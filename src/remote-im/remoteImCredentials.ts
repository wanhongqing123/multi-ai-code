import type { RemoteImAccountConfig } from '../../electron/preload.js'

export interface RemoteImCredentialPreset {
  id: string
  label: string
  sdkAppId: number
  userSigSecretKey: string
}

// 只保留一套凭证，且必须与 MaiChat 的 RemoteIMCredentialDefaults 完全一致
// （同 sdkAppId、同 secretKey）。多一套的代价不是"多个选项"而是静默失联：
// 两端凭证不同就是两个不同的腾讯云应用，IM 和 TRTC 都碰不到一起，
// 表现为消息发不出去、远程桌面进不了同一个房间，却没有任何报错。
export const REMOTE_IM_CREDENTIAL_PRESETS: RemoteImCredentialPreset[] = [
  {
    id: 'tencent-im-1600148979',
    label: '测试凭证 1600148979',
    sdkAppId: 1600148979,
    userSigSecretKey: 'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
  }
]

export const DEFAULT_REMOTE_IM_CREDENTIAL_PRESET = REMOTE_IM_CREDENTIAL_PRESETS[0]

export function getSelectedRemoteImCredentialPresetId(account: RemoteImAccountConfig): string {
  return (
    REMOTE_IM_CREDENTIAL_PRESETS.find(
      (preset) =>
        preset.sdkAppId === account.sdkAppId &&
        preset.userSigSecretKey === account.userSigSecretKey &&
        account.userSigMode === 'secret-key'
    )?.id ?? ''
  )
}

export function applyDefaultRemoteImCredential(
  account: RemoteImAccountConfig
): RemoteImAccountConfig {
  return applyRemoteImCredentialPreset(account, DEFAULT_REMOTE_IM_CREDENTIAL_PRESET.id)
}

export function applyRemoteImCredentialPreset(
  account: RemoteImAccountConfig,
  presetId: string
): RemoteImAccountConfig {
  const preset = REMOTE_IM_CREDENTIAL_PRESETS.find((item) => item.id === presetId)
  if (!preset) return account
  return {
    ...account,
    sdkAppId: preset.sdkAppId,
    userSigMode: 'secret-key',
    userSigEndpoint: '',
    userSigSecretKey: preset.userSigSecretKey
  }
}
