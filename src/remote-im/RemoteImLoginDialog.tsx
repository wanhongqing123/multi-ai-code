import { type FormEvent, useEffect, useRef, useState } from 'react'
import type { RemoteImAccountConfig, RemoteImLoginState } from '../../electron/preload.js'
import {
  applyDefaultRemoteImCredential,
  DEFAULT_REMOTE_IM_CREDENTIAL_PRESET
} from './remoteImCredentials.js'

export interface RemoteImLoginSubmitInput {
  account: RemoteImAccountConfig
}

export interface RemoteImLoginDialogProps {
  open: boolean
  loginState: RemoteImLoginState | null
  saving: boolean
  error: string | null
  onLookupAccount?: (userId: string) => Promise<RemoteImAccountConfig | null>
  onClose: () => void
  onSubmit: (input: RemoteImLoginSubmitInput) => void
}

const EMPTY_ACCOUNT: RemoteImAccountConfig = {
  provider: 'tencent-im',
  sdkAppId: DEFAULT_REMOTE_IM_CREDENTIAL_PRESET.sdkAppId,
  desktopUserId: '',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: DEFAULT_REMOTE_IM_CREDENTIAL_PRESET.userSigSecretKey,
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: []
}

function toFixedCredentialAccount(account: RemoteImAccountConfig): RemoteImAccountConfig {
  return {
    ...applyDefaultRemoteImCredential(account),
    desktopRole: 'master'
  }
}

export function applyLoadedRemoteImLoginAccount(
  draft: RemoteImAccountConfig,
  account: RemoteImAccountConfig
): RemoteImAccountConfig {
  return account.desktopUserId.trim() === draft.desktopUserId.trim()
    ? toFixedCredentialAccount(account)
    : draft
}

export default function RemoteImLoginDialog(props: RemoteImLoginDialogProps): JSX.Element | null {
  const [draft, setDraft] = useState<RemoteImAccountConfig>(
    props.loginState?.account ?? EMPTY_ACCOUNT
  )
  const loadedAccountUserIdRef = useRef<string>('')

  useEffect(() => {
    if (!props.open) return
    const account = toFixedCredentialAccount(props.loginState?.account ?? EMPTY_ACCOUNT)
    setDraft(account)
    loadedAccountUserIdRef.current = account.desktopUserId.trim()
  }, [props.open, props.loginState])

  useEffect(() => {
    if (!props.open || !props.onLookupAccount) return
    const userId = draft.desktopUserId.trim()
    if (!userId || loadedAccountUserIdRef.current === userId) return
    let cancelled = false
    const timer = window.setTimeout(() => {
      void props.onLookupAccount?.(userId).then((account) => {
        if (cancelled) return
        loadedAccountUserIdRef.current = userId
        if (!account) return
        setDraft((current) => applyLoadedRemoteImLoginAccount(current, account))
      })
    }, 250)
    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [props.open, props.onLookupAccount, draft.desktopUserId])

  if (!props.open) return null

  function update(next: Partial<RemoteImAccountConfig>): void {
    setDraft((current) => ({ ...current, ...next }))
  }

  function handleSubmit(event: FormEvent): void {
    event.preventDefault()
    const userId = draft.desktopUserId.trim()
    if (!userId) return
    props.onSubmit({
      account: toFixedCredentialAccount({
        ...draft,
        desktopUserId: userId
      })
    })
  }

  return (
    <div className="modal-backdrop" data-close-on-backdrop="false">
      <form className="modal remote-im-login-modal" onSubmit={handleSubmit} onClick={(event) => event.stopPropagation()}>
        <header className="remote-im-login-header">
          <div>
            <h2>IM 登录</h2>
            <p>登录后可通过可信好友收发消息，并把好友消息交给当前 AICLI 处理。</p>
          </div>
          <button type="button" className="remote-im-close" onClick={props.onClose}>
            ×
          </button>
        </header>

        <div className="remote-im-login-grid">
          <label className="remote-im-login-full">
            UserID
            <input
              value={draft.desktopUserId}
              onChange={(event) => {
                const userId = event.currentTarget.value
                update({ desktopUserId: userId })
              }}
              placeholder="test123"
            />
          </label>
          <div className="remote-im-login-fixed remote-im-login-full">
            <span>基础 IM 配置固定</span>
            <strong>SDKAppID {DEFAULT_REMOTE_IM_CREDENTIAL_PRESET.sdkAppId}</strong>
            <small>SecretKey 使用内置测试凭证，不在设置界面修改。</small>
          </div>
        </div>

        {props.error ? <div className="remote-im-login-error">{props.error}</div> : null}

        <footer className="remote-im-login-actions">
          <button type="button" onClick={props.onClose} disabled={props.saving}>
            取消
          </button>
          <button type="submit" disabled={props.saving || !draft.desktopUserId.trim()}>
            {props.saving ? '登录中...' : '登录'}
          </button>
        </footer>
      </form>
    </div>
  )
}
