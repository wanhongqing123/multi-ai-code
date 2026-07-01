import type { RemoteImAccountConfig } from '../../electron/preload.js'

export interface RemoteImCredentialPreset {
  id: string
  label: string
  sdkAppId: number
  userSigSecretKey: string
}

export const REMOTE_IM_CREDENTIAL_PRESETS: RemoteImCredentialPreset[] = [
  {
    id: 'tencent-im-1600148979',
    label: '测试凭证 1600148979',
    sdkAppId: 1600148979,
    userSigSecretKey: 'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
  },
  {
    id: 'tencent-im-1400704311',
    label: '测试凭证 1400704311',
    sdkAppId: 1400704311,
    userSigSecretKey: '8b897045d1ee4f067a745b1b6a3fb834d1bd4c5951de43282c21b945f98ec982'
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
