import { type FormEvent, useEffect, useRef, useState } from 'react'
import type {
  RemoteImAccountConfig,
  RemoteImConfig,
  RemoteImLoginState
} from '../../electron/preload.js'
import {
  applyDefaultRemoteImCredential,
  DEFAULT_REMOTE_IM_CREDENTIAL_PRESET
} from './remoteImCredentials.js'

export interface RemoteImLoginSubmitInput {
  account: RemoteImAccountConfig
  projectConfig?: RemoteImConfig
}

export interface RemoteImLoginDialogProps {
  open: boolean
  loginState: RemoteImLoginState | null
  projectConfig: RemoteImConfig | null
  projectConfigReady: boolean
  saving: boolean
  error: string | null
  onLookupAccount?: (userId: string) => Promise<RemoteImAccountConfig | null>
  onClose: () => void
  onSubmit: (input: RemoteImLoginSubmitInput) => void
  /**
   * 'modal'（默认）：浮在应用之上的弹窗，带遮罩与取消/关闭。
   * 'gate'：作为启动首屏的独立全屏登录页，无遮罩、无取消/关闭（登录是进入应用的前置）。
   */
  variant?: 'modal' | 'gate'
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

function readNumberInput(value: string, fallback: number): number {
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : fallback
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
  const [projectDraft, setProjectDraft] = useState<RemoteImConfig | null>(
    props.projectConfig
  )
  const loadedAccountUserIdRef = useRef<string>('')

  useEffect(() => {
    if (!props.open) return
    const account = toFixedCredentialAccount(props.loginState?.account ?? EMPTY_ACCOUNT)
    setDraft(account)
    loadedAccountUserIdRef.current = account.desktopUserId.trim()
  }, [props.open, props.loginState])

  useEffect(() => {
    if (!props.open) return
    setProjectDraft(props.projectConfig)
  }, [props.open, props.projectConfig])

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

  function updateProject(next: Partial<RemoteImConfig>): void {
    setProjectDraft((current) => current ? { ...current, ...next } : current)
  }

  function handleSubmit(event: FormEvent): void {
    event.preventDefault()
    const userId = draft.desktopUserId.trim()
    if (!userId) return
    props.onSubmit({
      account: toFixedCredentialAccount({
        ...draft,
        desktopUserId: userId
      }),
      projectConfig: projectDraft ? { ...projectDraft, enabled: true } : undefined
    })
  }

  const isGate = props.variant === 'gate'

  return (
    <div className={isGate ? 'remote-im-login-gate' : 'modal-backdrop'} data-close-on-backdrop="false">
      {isGate ? (
        <div className="remote-im-login-bg" aria-hidden>
          <span className="remote-im-login-bg-blob remote-im-login-bg-blob-a" />
          <span className="remote-im-login-bg-blob remote-im-login-bg-blob-b" />
          <span className="remote-im-login-bg-blob remote-im-login-bg-blob-c" />
        </div>
      ) : null}
      <form className="modal remote-im-login-modal" onSubmit={handleSubmit} onClick={(event) => event.stopPropagation()}>
        {isGate ? null : (
          <button type="button" className="remote-im-close remote-im-login-close" onClick={props.onClose}>
            ×
          </button>
        )}

        {isGate ? <div className="remote-im-login-drag" aria-hidden /> : null}

        <div className="remote-im-login-brand">
          <span className="remote-im-login-logo" aria-hidden>
            M
          </span>
          <h2>欢迎使用 Multi-AI Code</h2>
        </div>

        <div className="remote-im-login-fields">
          <input
            value={draft.desktopUserId}
            autoFocus
            onChange={(event) => {
              const userId = event.currentTarget.value
              update({ desktopUserId: userId })
            }}
            placeholder="请输入登录账号"
          />
        </div>

        {props.error ? <div className="remote-im-login-error">{props.error}</div> : null}

        <button type="submit" className="remote-im-login-submit" disabled={props.saving || !draft.desktopUserId.trim()}>
          {props.saving ? '登录中...' : '登录'}
        </button>

        {projectDraft ? (
          <details className="remote-im-login-advanced">
            <summary>当前项目 IM 配置</summary>
            <p className="remote-im-login-advanced-hint">登录后好友消息会接入本机 AICLI，这里只配置 AICLI 输出回传频率。</p>
            <div className="remote-im-login-advanced-grid">
              <label>
                AI 输出回传间隔
                <input
                  type="number"
                  min={1000}
                  max={30000}
                  step={500}
                  value={projectDraft.outputFlushIntervalMs}
                  disabled={!props.projectConfigReady}
                  onChange={(event) =>
                    updateProject({
                      outputFlushIntervalMs: readNumberInput(
                        event.currentTarget.value,
                        projectDraft.outputFlushIntervalMs
                      )
                    })
                  }
                />
                <small>每隔这段时间合并一次 AICLI 新输出再发回 IM，避免刷屏。</small>
              </label>
              <label>
                单次回传字符数
                <input
                  type="number"
                  min={200}
                  max={4000}
                  step={100}
                  value={projectDraft.outputMaxChunkChars}
                  disabled={!props.projectConfigReady}
                  onChange={(event) =>
                    updateProject({
                      outputMaxChunkChars: readNumberInput(
                        event.currentTarget.value,
                        projectDraft.outputMaxChunkChars
                      )
                    })
                  }
                />
                <small>单条 IM 最多携带的 AICLI 输出长度，超出后会分段发送。</small>
              </label>
            </div>
          </details>
        ) : null}
      </form>
    </div>
  )
}
