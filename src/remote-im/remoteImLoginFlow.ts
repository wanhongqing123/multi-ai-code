import type { RemoteImAccountConfig, RemoteImLoginState } from '../../electron/preload.js'

export function isRemoteImAccountReady(account: RemoteImAccountConfig | null | undefined): boolean {
  if (!account) return false
  if (!account.desktopUserId.trim()) return false
  if (!account.sdkAppId) return false
  if (account.userSigMode === 'secret-key') return Boolean(account.userSigSecretKey.trim())
  return Boolean(account.userSigEndpoint.trim())
}

export function shouldPromptRemoteImStartupLogin(
  loginState: RemoteImLoginState | null | undefined
): boolean {
  return !isRemoteImAccountReady(loginState?.account)
}
