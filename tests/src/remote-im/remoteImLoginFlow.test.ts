import { describe, expect, it } from 'vitest'
import type { RemoteImAccountConfig, RemoteImLoginState } from '../../../electron/preload.js'
import {
  isRemoteImAccountReady,
  shouldPromptRemoteImStartupLogin
} from '../../../src/remote-im/remoteImLoginFlow.js'

const account: RemoteImAccountConfig = {
  provider: 'tencent-im',
  sdkAppId: 1600148979,
  desktopUserId: 'mac-quark-pc',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: []
}

function loginState(nextAccount: RemoteImAccountConfig): RemoteImLoginState {
  return {
    profileId: nextAccount.desktopUserId,
    account: nextAccount
  }
}

describe('remoteImLoginFlow', () => {
  it('treats a complete saved secret-key account as ready for automatic connection', () => {
    expect(isRemoteImAccountReady(account)).toBe(true)
    expect(shouldPromptRemoteImStartupLogin(loginState(account))).toBe(false)
  })

  it('prompts startup login when identity or credentials are missing', () => {
    expect(shouldPromptRemoteImStartupLogin(null)).toBe(true)
    expect(shouldPromptRemoteImStartupLogin(loginState({ ...account, desktopUserId: '' }))).toBe(
      true
    )
    expect(shouldPromptRemoteImStartupLogin(loginState({ ...account, userSigSecretKey: '' }))).toBe(
      true
    )
  })

  it('accepts endpoint accounts only after an endpoint is configured', () => {
    expect(
      isRemoteImAccountReady({
        ...account,
        userSigMode: 'endpoint',
        userSigSecretKey: '',
        userSigEndpoint: ''
      })
    ).toBe(false)
    expect(
      isRemoteImAccountReady({
        ...account,
        userSigMode: 'endpoint',
        userSigSecretKey: '',
        userSigEndpoint: 'https://example.test/usersig'
      })
    ).toBe(true)
  })
})
