import { type FormEvent, useMemo, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type {
  RemoteImConfig,
  RemoteImContactRelation,
  RemoteImMessage,
  RemoteImStatus
} from '../../electron/preload.js'
import {
  filterRemoteImMessagesByPeer,
  formatRemoteImTime,
  getRemoteImConversations,
  getRemoteImMessageDisplayMeta,
  getRemoteImMessageStatusLabel,
  getRemoteImStatusLabel,
  isRemoteImSendDisabled
} from './remoteImViewModel.js'

export interface RemoteImDrawerProps {
  open: boolean
  projectId: string | null
  sessionRunning: boolean
  status: RemoteImStatus | null
  config: RemoteImConfig
  messages: RemoteImMessage[]
  selectedPeerUserId: string | null
  input: string
  onInputChange: (value: string) => void
  onSelectPeer: (userId: string) => void
  onSend: (toUserId: string) => void
  onAddContact: (relation: RemoteImContactRelation, userId: string) => void
  onClear: () => void
  onLoginClick: () => void
  onClose: () => void
}

type ConversationFilter = 'recent' | RemoteImContactRelation

const RELATION_FILTERS: Array<{ value: ConversationFilter; label: string }> = [
  { value: 'recent', label: '最近' },
  { value: 'friend', label: '好友' },
  { value: 'master', label: '主人' },
  { value: 'slave', label: '奴隶' }
]

function getRelationLabel(relation: RemoteImContactRelation): string {
  if (relation === 'friend') return '好友'
  if (relation === 'master') return '主人'
  return '奴隶'
}

function RemoteImMarkdown(props: { content: string }): JSX.Element {
  return (
    <div className="remote-im-markdown">
      <ReactMarkdown remarkPlugins={[remarkGfm]}>{props.content.trim()}</ReactMarkdown>
    </div>
  )
}

export default function RemoteImDrawer(props: RemoteImDrawerProps): JSX.Element | null {
  const [conversationFilter, setConversationFilter] = useState<ConversationFilter>('recent')
  const [newContactRelation, setNewContactRelation] = useState<RemoteImContactRelation>('friend')
  const [newContactUserId, setNewContactUserId] = useState('')

  const conversations = useMemo(
    () => getRemoteImConversations(props.config, props.messages),
    [props.config, props.messages]
  )
  const filteredConversations =
    conversationFilter === 'recent'
      ? conversations
      : conversations.filter((conversation) => conversation.relation === conversationFilter)
  const selectedPeerUserId =
    props.selectedPeerUserId ?? filteredConversations[0]?.userId ?? conversations[0]?.userId ?? null
  const selectedMessages = selectedPeerUserId
    ? filterRemoteImMessagesByPeer(props.messages, props.config.desktopUserId, selectedPeerUserId)
    : []
  const selectedConversation = selectedPeerUserId
    ? conversations.find((conversation) => conversation.userId === selectedPeerUserId)
    : null
  const slaveMode = props.config.desktopRole === 'slave'
  const inputDisabled =
    slaveMode || !selectedPeerUserId || !props.projectId || props.status?.state !== 'connected'
  const clearDisabled = !props.projectId || props.messages.length === 0
  const sendDisabled =
    isRemoteImSendDisabled({
      projectId: props.projectId,
      sessionRunning: props.sessionRunning,
      text: props.input,
      status: props.status,
      desktopRole: props.config.desktopRole
    }) || !selectedPeerUserId
  const loggedInUserId = props.config.desktopUserId.trim()

  if (!props.open) return null

  function handleSubmit(event: FormEvent): void {
    event.preventDefault()
    if (!sendDisabled && selectedPeerUserId) props.onSend(selectedPeerUserId)
  }

  function handleAddContact(event: FormEvent): void {
    event.preventDefault()
    const userId = newContactUserId.trim()
    if (!userId) return
    props.onAddContact(newContactRelation, userId)
    setNewContactUserId('')
  }

  return (
    <aside className="remote-im-drawer" aria-label="远程 IM">
      <div className="remote-im-panel">
        <header className="remote-im-header">
          <div className="remote-im-title">远程 IM</div>
          <div
            className={`remote-im-status status-${props.status?.state ?? 'disconnected'}`}
            title={props.status?.detail ?? undefined}
          >
            <span />
            {getRemoteImStatusLabel(props.status)}
            {props.status?.detail ? (
              <small className="remote-im-status-detail">{props.status.detail}</small>
            ) : null}
          </div>
          <button
            type="button"
            className="remote-im-login-action"
            onClick={props.onLoginClick}
            title="登录远程 IM 账号"
          >
            {loggedInUserId
              ? `${loggedInUserId} · ${props.status?.state === 'connected' ? '重新登录' : '登录'}`
              : '登录'}
          </button>
          <button
            type="button"
            className="remote-im-clear"
            aria-label="清空远程 IM 消息"
            disabled={clearDisabled}
            onClick={props.onClear}
          >
            Clear
          </button>
          <button
            type="button"
            className="remote-im-close"
            aria-label="关闭远程 IM 会话"
            onClick={props.onClose}
          >
            ×
          </button>
        </header>

        <div className="remote-im-shell">
          <section className="remote-im-sidebar" aria-label="会话">
            <div className="remote-im-sidebar-head">
              <strong>会话</strong>
              <span>{props.config.desktopUserId || '未设置 UserID'}</span>
            </div>
            <div className="remote-im-relation-tabs" role="tablist" aria-label="联系人关系">
              {RELATION_FILTERS.map((filter) => (
                <button
                  key={filter.value}
                  type="button"
                  data-relation={filter.value}
                  className={conversationFilter === filter.value ? 'active' : ''}
                  onClick={() => setConversationFilter(filter.value)}
                >
                  {filter.label}
                </button>
              ))}
            </div>

            <div className="remote-im-conversations">
              {filteredConversations.length === 0 ? (
                <div className="remote-im-conversation-empty">暂无会话</div>
              ) : (
                filteredConversations.map((conversation) => (
                  <button
                    key={conversation.userId}
                    type="button"
                    data-relation={conversation.relation}
                    className={`remote-im-conversation ${
                      conversation.userId === selectedPeerUserId ? 'active' : ''
                    }`}
                    onClick={() => props.onSelectPeer(conversation.userId)}
                  >
                    <div>
                      <strong>{conversation.userId}</strong>
                      <span>{conversation.lastMessagePreview || '暂无消息'}</span>
                    </div>
                    <em>{getRelationLabel(conversation.relation)}</em>
                  </button>
                ))
              )}
            </div>

            <form className="remote-im-add-contact" aria-label="添加联系人" onSubmit={handleAddContact}>
              <div className="remote-im-add-title">添加联系人</div>
              <select
                name="relation"
                value={newContactRelation}
                onChange={(event) =>
                  setNewContactRelation(event.currentTarget.value as RemoteImContactRelation)
                }
              >
                <option value="friend">好友</option>
                <option value="master">主人</option>
                <option value="slave">奴隶</option>
              </select>
              <input
                name="userId"
                value={newContactUserId}
                onChange={(event) => setNewContactUserId(event.currentTarget.value)}
                placeholder="UserID"
              />
              <button type="submit">添加</button>
            </form>
          </section>

          <section className="remote-im-chat" aria-label="聊天">
            <div className="remote-im-chat-head">
              <div>
                <span>当前会话</span>
                <strong className="remote-im-chat-title">
                  {selectedPeerUserId || '未选择联系人'}
                </strong>
              </div>
              {selectedConversation ? <em>{getRelationLabel(selectedConversation.relation)}</em> : null}
            </div>

            <div className="remote-im-messages">
              {selectedMessages.length === 0 ? (
                <div className="remote-im-empty">还没有远程 IM 消息。</div>
              ) : (
                selectedMessages.map((message) => {
                  const statusLabel = getRemoteImMessageStatusLabel(message)
                  const displayMeta = getRemoteImMessageDisplayMeta(props.config, message)
                  return (
                    <article
                      key={message.id}
                      className={`remote-im-message role-${message.role} status-${message.status}`}
                    >
                      <div className="remote-im-bubble-wrap">
                        <div className="remote-im-message-meta">
                          <strong>{displayMeta.userId}</strong>
                          <em data-message-relation={displayMeta.relation}>
                            {getRelationLabel(displayMeta.relation)}
                          </em>
                          <span>{formatRemoteImTime(message.createdAt)}</span>
                        </div>
                        <div className="remote-im-bubble">
                          <RemoteImMarkdown content={message.content} />
                          {statusLabel ? (
                            <span className="remote-im-message-status">{statusLabel}</span>
                          ) : null}
                        </div>
                      </div>
                    </article>
                  )
                })
              )}
            </div>

            <form className="remote-im-composer" onSubmit={handleSubmit}>
              <input
                value={props.input}
                onChange={(event) => props.onInputChange(event.currentTarget.value)}
                disabled={inputDisabled}
                placeholder={
                  slaveMode
                    ? '奴隶模式：等待主人发送任务'
                    : selectedPeerUserId
                      ? '输入要发送给当前 UserID 的消息...'
                      : '先选择一个联系人'
                }
              />
              <button type="submit" disabled={sendDisabled}>
                发送
              </button>
            </form>
          </section>
        </div>
      </div>
    </aside>
  )
}
