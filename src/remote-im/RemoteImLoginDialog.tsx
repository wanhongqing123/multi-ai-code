import { type FormEvent, useEffect, useRef, useState } from 'react'
import type { RemoteImAccountConfig, RemoteImLoginState } from '../../electron/preload.js'
import {
  applyRemoteImCredentialPreset,
  getSelectedRemoteImCredentialPresetId,
  REMOTE_IM_CREDENTIAL_PRESETS
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
  sdkAppId: null,
  desktopUserId: '',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: '',
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: []
}

export function applyLoadedRemoteImLoginAccount(
  draft: RemoteImAccountConfig,
  account: RemoteImAccountConfig
): RemoteImAccountConfig {
  return account.desktopUserId.trim() === draft.desktopUserId.trim() ? account : draft
}

export default function RemoteImLoginDialog(props: RemoteImLoginDialogProps): JSX.Element | null {
  const [draft, setDraft] = useState<RemoteImAccountConfig>(
    props.loginState?.account ?? EMPTY_ACCOUNT
  )
  const loadedAccountUserIdRef = useRef<string>('')

  useEffect(() => {
    if (!props.open) return
    const account = props.loginState?.account ?? EMPTY_ACCOUNT
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
      account: {
        ...draft,
        desktopUserId: userId
      }
    })
  }

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <form className="modal remote-im-login-modal" onSubmit={handleSubmit} onClick={(event) => event.stopPropagation()}>
        <header className="remote-im-login-header">
          <div>
            <h2>IM 登录</h2>
            <p>填写 UserID 和凭证后点击登录，程序启动时不会自动连接 IM。</p>
          </div>
          <button type="button" className="remote-im-close" onClick={props.onClose}>
            ×
          </button>
        </header>

        <div className="remote-im-login-grid">
          <label>
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
          <label>
            角色
            <select
              value={draft.desktopRole}
              onChange={(event) =>
                update({ desktopRole: event.currentTarget.value as RemoteImAccountConfig['desktopRole'] })
              }
            >
              <option value="master">主人</option>
              <option value="slave">奴隶</option>
            </select>
          </label>
          <label>
            凭证预设
            <select
              value={getSelectedRemoteImCredentialPresetId(draft)}
              onChange={(event) =>
                setDraft(applyRemoteImCredentialPreset(draft, event.currentTarget.value))
              }
            >
              <option value="">自定义</option>
              {REMOTE_IM_CREDENTIAL_PRESETS.map((preset) => (
                <option key={preset.id} value={preset.id}>
                  {preset.label}
                </option>
              ))}
            </select>
          </label>
          <label>
            SDKAppID
            <input
              value={draft.sdkAppId ?? ''}
              onChange={(event) =>
                update({
                  sdkAppId: event.currentTarget.value ? Number(event.currentTarget.value) : null
                })
              }
              placeholder="1400704311"
            />
          </label>
          <label>
            UserSig 方式
            <select
              value={draft.userSigMode}
              onChange={(event) =>
                update({
                  userSigMode: event.currentTarget.value as RemoteImAccountConfig['userSigMode']
                })
              }
            >
              <option value="secret-key">本地 SecretKey(测试)</option>
              <option value="endpoint">服务端 endpoint</option>
            </select>
          </label>
          {draft.userSigMode === 'secret-key' ? (
            <label className="remote-im-login-full">
              SecretKey
              <input
                type="password"
                value={draft.userSigSecretKey}
                onChange={(event) => update({ userSigSecretKey: event.currentTarget.value })}
                placeholder="填入 IM 应用 SecretKey"
              />
            </label>
          ) : (
            <label className="remote-im-login-full">
              UserSig endpoint
              <input
                value={draft.userSigEndpoint}
                onChange={(event) => update({ userSigEndpoint: event.currentTarget.value })}
                placeholder="https://example.com/tencent-im/usersig"
              />
            </label>
          )}
        </div>

        {props.error ? <div className="remote-im-login-error">{props.error}</div> : null}

        <footer className="remote-im-login-actions">
          <button type="button" onClick={props.onClose} disabled={props.saving}>
            取消
          </button>
          <button type="submit" disabled={props.saving || !draft.desktopUserId.trim()}>
            {props.saving ? '登录中...' : '登录并连接'}
          </button>
        </footer>
      </form>
    </div>
  )
}
