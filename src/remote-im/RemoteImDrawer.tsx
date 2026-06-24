import type { FormEvent } from 'react'
import type { RemoteImMessage, RemoteImStatus } from '../../electron/preload.js'
import {
  formatRemoteImTime,
  getRemoteImMessageAuthor,
  getRemoteImMessageAvatar,
  getRemoteImMessageStatusLabel,
  getRemoteImStatusLabel,
  isRemoteImSendDisabled
} from './remoteImViewModel.js'

export interface RemoteImDrawerProps {
  open: boolean
  projectId: string | null
  sessionRunning: boolean
  status: RemoteImStatus | null
  messages: RemoteImMessage[]
  input: string
  onInputChange: (value: string) => void
  onSend: () => void
  onClose: () => void
}

export default function RemoteImDrawer(props: RemoteImDrawerProps): JSX.Element | null {
  if (!props.open) return null
  const sendDisabled = isRemoteImSendDisabled({
    projectId: props.projectId,
    sessionRunning: props.sessionRunning,
    text: props.input,
    status: props.status
  })

  function handleSubmit(event: FormEvent): void {
    event.preventDefault()
    if (!sendDisabled) props.onSend()
  }

  return (
    <aside className="remote-im-drawer" aria-label="远程 IM">
      <div className="remote-im-panel">
        <header className="remote-im-header">
          <div className="remote-im-title">远程 IM</div>
          <div className={`remote-im-status status-${props.status?.state ?? 'disconnected'}`}>
            <span />
            {getRemoteImStatusLabel(props.status)}
          </div>
          <button
            type="button"
            className="remote-im-close"
            aria-label="关闭远程 IM 会话"
            onClick={props.onClose}
          >
            ×
          </button>
        </header>

        <div className="remote-im-messages">
          {props.messages.length === 0 ? (
            <div className="remote-im-empty">还没有远程 IM 消息。</div>
          ) : (
            props.messages.map((message) => (
              <article
                key={message.id}
                className={`remote-im-message role-${message.role} status-${message.status}`}
              >
                <div className="remote-im-avatar">{getRemoteImMessageAvatar(message)}</div>
                <div className="remote-im-bubble-wrap">
                  <div className="remote-im-message-meta">
                    <strong>{getRemoteImMessageAuthor(message)}</strong>
                    <span>{formatRemoteImTime(message.createdAt)}</span>
                  </div>
                  <div className="remote-im-bubble">
                    <div>{message.content}</div>
                    <span className="remote-im-message-status">
                      {getRemoteImMessageStatusLabel(message)}
                    </span>
                  </div>
                </div>
              </article>
            ))
          )}
        </div>

        <form className="remote-im-composer" onSubmit={handleSubmit}>
          <input
            value={props.input}
            onChange={(event) => props.onInputChange(event.currentTarget.value)}
            placeholder="输入要发送给 AICLI 的消息..."
          />
          <button type="submit" disabled={sendDisabled}>
            发送
          </button>
        </form>
      </div>
    </aside>
  )
}
