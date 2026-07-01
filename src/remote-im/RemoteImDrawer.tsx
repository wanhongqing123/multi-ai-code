import {
  type CSSProperties,
  type FormEvent,
  type PointerEvent,
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState
} from 'react'
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
  getRemoteImMessageStatusTitle,
  getRemoteImStatusLabel,
  isRemoteImSendDisabled
} from './remoteImViewModel.js'
import {
  clampRemoteImPanelPosition,
  getDraggedRemoteImPanelPosition,
  getInitialRemoteImPanelPosition,
  type RemoteImPanelFrame,
  type RemoteImPanelPosition
} from './remoteImDrag.js'

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
  onDeleteContact: (userId: string) => void
  onClear: () => void
  onClose: () => void
}

type ConversationFilter = 'recent' | 'friend'

const RELATION_FILTERS: Array<{ value: ConversationFilter; label: string }> = [
  { value: 'recent', label: '最近' },
  { value: 'friend', label: '好友' }
]

function getRelationLabel(relation: RemoteImContactRelation): string {
  void relation
  return '好友'
}

function RemoteImMarkdown(props: { content: string }): JSX.Element {
  return (
    <div className="remote-im-markdown">
      <ReactMarkdown remarkPlugins={[remarkGfm]}>{props.content.trim()}</ReactMarkdown>
    </div>
  )
}

function getRemoteImPanelFrame(panel: HTMLElement): RemoteImPanelFrame {
  const rect = panel.getBoundingClientRect()
  return {
    viewportWidth: window.innerWidth,
    viewportHeight: window.innerHeight,
    panelWidth: rect.width,
    panelHeight: rect.height
  }
}

function isInteractiveDragTarget(target: EventTarget | null): boolean {
  return target instanceof HTMLElement && Boolean(target.closest('button, input, select, textarea, a'))
}

export default function RemoteImDrawer(props: RemoteImDrawerProps): JSX.Element | null {
  const [conversationFilter, setConversationFilter] = useState<ConversationFilter>('recent')
  const [newContactUserId, setNewContactUserId] = useState('')
  const panelRef = useRef<HTMLDivElement | null>(null)
  const [panelPosition, setPanelPosition] = useState<RemoteImPanelPosition | null>(null)
  const [dragState, setDragState] = useState<{
    startPosition: RemoteImPanelPosition
    startPointer: RemoteImPanelPosition
  } | null>(null)

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
  const inputDisabled =
    !selectedPeerUserId || !props.projectId || props.status?.state !== 'connected'
  const clearDisabled = !props.projectId || props.messages.length === 0
  const sendDisabled =
    isRemoteImSendDisabled({
      projectId: props.projectId,
      sessionRunning: props.sessionRunning,
      text: props.input,
      status: props.status,
      desktopRole: props.config.desktopRole
    }) || !selectedPeerUserId
  useEffect(() => {
    if (!props.open) return
    const panel = panelRef.current
    if (!panel) return
    setPanelPosition((current) => {
      const frame = getRemoteImPanelFrame(panel)
      return current
        ? clampRemoteImPanelPosition(current, frame)
        : getInitialRemoteImPanelPosition(frame)
    })
  }, [props.open])

  useEffect(() => {
    if (!props.open || !panelPosition) return

    function handleResize(): void {
      const panel = panelRef.current
      if (!panel) return
      setPanelPosition((current) =>
        current ? clampRemoteImPanelPosition(current, getRemoteImPanelFrame(panel)) : current
      )
    }

    window.addEventListener('resize', handleResize)
    return () => window.removeEventListener('resize', handleResize)
  }, [props.open, panelPosition])

  useEffect(() => {
    if (!dragState) return
    const activeDragState = dragState

    function handlePointerMove(event: globalThis.PointerEvent): void {
      const panel = panelRef.current
      if (!panel) return
      setPanelPosition(
        getDraggedRemoteImPanelPosition({
          startPosition: activeDragState.startPosition,
          startPointer: activeDragState.startPointer,
          currentPointer: { x: event.clientX, y: event.clientY },
          frame: getRemoteImPanelFrame(panel)
        })
      )
    }

    function handlePointerUp(): void {
      setDragState(null)
    }

    window.addEventListener('pointermove', handlePointerMove)
    window.addEventListener('pointerup', handlePointerUp)
    window.addEventListener('pointercancel', handlePointerUp)
    return () => {
      window.removeEventListener('pointermove', handlePointerMove)
      window.removeEventListener('pointerup', handlePointerUp)
      window.removeEventListener('pointercancel', handlePointerUp)
    }
  }, [dragState])

  const handleDragStart = useCallback(
    (event: PointerEvent<HTMLElement>) => {
      if (event.button !== 0 || isInteractiveDragTarget(event.target)) return
      const panel = panelRef.current
      if (!panel) return
      const frame = getRemoteImPanelFrame(panel)
      const startPosition =
        panelPosition ?? getInitialRemoteImPanelPosition(frame)
      event.preventDefault()
      setPanelPosition(startPosition)
      setDragState({
        startPosition,
        startPointer: { x: event.clientX, y: event.clientY }
      })
    },
    [panelPosition]
  )

  const panelStyle: CSSProperties | undefined = panelPosition
    ? {
        transform: `translate3d(${Math.round(panelPosition.x)}px, ${Math.round(panelPosition.y)}px, 0)`
      }
    : undefined

  if (!props.open) return null

  function handleSubmit(event: FormEvent): void {
    event.preventDefault()
    if (!sendDisabled && selectedPeerUserId) props.onSend(selectedPeerUserId)
  }

  function handleAddContact(event: FormEvent): void {
    event.preventDefault()
    const userId = newContactUserId.trim()
    if (!userId) return
    props.onAddContact('friend', userId)
    setNewContactUserId('')
  }

  return (
    <aside className="remote-im-drawer" aria-label="远程 IM">
      <div
        ref={panelRef}
        className={`remote-im-panel${dragState ? ' dragging' : ''}`}
        style={panelStyle}
      >
        <header
          className="remote-im-header"
          onPointerDown={handleDragStart}
          title="拖动移动远程 IM 窗口"
        >
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
          <span className="remote-im-account-label" title={props.config.desktopUserId || undefined}>
            {props.config.desktopUserId || '未登录'}
          </span>
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
                  <div
                    key={conversation.userId}
                    data-relation={conversation.relation}
                    className={`remote-im-conversation-row ${
                      conversation.userId === selectedPeerUserId ? 'active' : ''
                    }`}
                  >
                    <button
                      type="button"
                      className="remote-im-conversation"
                      onClick={() => props.onSelectPeer(conversation.userId)}
                    >
                      <div>
                        <strong>{conversation.userId}</strong>
                        <span>{conversation.lastMessagePreview || '暂无消息'}</span>
                      </div>
                      <em>{getRelationLabel(conversation.relation)}</em>
                    </button>
                    <button
                      type="button"
                      className="remote-im-delete-contact"
                      aria-label={`删除好友 ${conversation.userId} 及聊天历史`}
                      title="删除好友及聊天历史"
                      onClick={() => props.onDeleteContact(conversation.userId)}
                    >
                      ×
                    </button>
                  </div>
                ))
              )}
            </div>

            <form className="remote-im-add-contact" aria-label="添加联系人" onSubmit={handleAddContact}>
              <div className="remote-im-add-title">添加联系人</div>
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
                  const statusTitle = getRemoteImMessageStatusTitle(message)
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
                            <span className="remote-im-message-status" title={statusTitle}>
                              {statusLabel}
                            </span>
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
                  selectedPeerUserId
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
