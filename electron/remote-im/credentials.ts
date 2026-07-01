export interface RemoteImCredential {
  sdkAppId: number | null
  userSigSecretKey: string
}

const CURRENT_DEFAULT_CREDENTIAL: RemoteImCredential = {
  sdkAppId: 1600148979,
  userSigSecretKey: 'aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861'
}

const PREVIOUS_DEFAULT_CREDENTIAL: RemoteImCredential = {
  sdkAppId: 1400704311,
  userSigSecretKey: '8b897045d1ee4f067a745b1b6a3fb834d1bd4c5951de43282c21b945f98ec982'
}

export function migrateRemoteImCredential(
  credential: RemoteImCredential
): RemoteImCredential {
  const cleanSecretKey = credential.userSigSecretKey.trim()
  if (
    credential.sdkAppId === PREVIOUS_DEFAULT_CREDENTIAL.sdkAppId &&
    cleanSecretKey === PREVIOUS_DEFAULT_CREDENTIAL.userSigSecretKey
  ) {
    return { ...CURRENT_DEFAULT_CREDENTIAL }
  }
  return {
    sdkAppId: credential.sdkAppId,
    userSigSecretKey: cleanSecretKey
  }
}
