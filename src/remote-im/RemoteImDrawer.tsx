import {
  type CSSProperties,
  type ChangeEvent,
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
  onSendImage: (toUserId: string, file: File) => void
  onAddContact: (relation: RemoteImContactRelation, userId: string) => void
  onDeleteContact: (userId: string) => void
  onClose: () => void
  // 会话是否还可能有更早的历史（分页翻完置 false 后隐藏按钮）。
  canLoadEarlier?: boolean
  onLoadEarlier?: (peerUserId: string) => Promise<void> | void
}

type ConversationFilter = 'recent' | 'friend'

type FilePreviewState =
  | {
      status: 'loading'
      fileName: string
      mimeType: string | null
      content: string
      error: string | null
    }
  | {
      status: 'ready'
      fileName: string
      mimeType: string
      content: string
      error: null
    }
  | {
      status: 'error'
      fileName: string
      mimeType: string | null
      content: string
      error: string
    }

const RELATION_FILTERS: Array<{ value: ConversationFilter; label: string }> = [
  { value: 'recent', label: '最近' },
  { value: 'friend', label: '好友' }
]

/**
 * 会话行上的关系标签。
 *
 * 以前无条件返回「好友」，参数直接丢弃——任何给你发过消息的陌生账号
 * 都被标成好友，而同一行的消息标着「已拒绝」，两个信息互相打架。
 */
function getRelationLabel(isContact: boolean): string {
  return isContact ? '好友' : '陌生人'
}

function RemoteImMarkdown(props: { content: string }): JSX.Element {
  return (
    <div className="remote-im-markdown">
      <ReactMarkdown remarkPlugins={[remarkGfm]}>{props.content.trim()}</ReactMarkdown>
    </div>
  )
}

function getRemoteImImageSource(value: string | null | undefined): string | null {
  const source = value?.trim()
  if (!source) return null
  if (/^(https?:|data:image\/|blob:|file:)/i.test(source)) return source
  if (source.startsWith('/')) return `file://${source}`
  return source
}

function RemoteImImageMessage(props: { message: RemoteImMessage }): JSX.Element {
  const attachment = props.message.attachment?.type === 'image' ? props.message.attachment : null
  const imageSource = getRemoteImImageSource(
    attachment?.localPath ?? attachment?.thumbnailUrl ?? attachment?.remoteUrl
  )
  const fileName = attachment?.fileName ?? props.message.content.replace(/^\[图片消息\]\s*/, '')

  return (
    <div className="remote-im-image-message">
      {imageSource ? (
        <img
          className="remote-im-image-preview"
          src={imageSource}
          alt={fileName || '图片消息'}
          loading="lazy"
        />
      ) : (
        <div className="remote-im-image-placeholder">图片暂不可预览</div>
      )}
      <div className="remote-im-image-caption">{fileName || props.message.content}</div>
    </div>
  )
}

function RemoteImFileMessage(props: {
  message: RemoteImMessage
  onPreview: (message: RemoteImMessage) => void
}): JSX.Element {
  const attachment = props.message.attachment?.type === 'file' ? props.message.attachment : null
  const fileName = attachment?.fileName ?? props.message.content.replace(/^\[文件消息\]\s*/, '')
  const lowerName = (fileName || '').toLowerCase()
  // 仅 md/html 支持内嵌预览；普通文件显示为文件卡片。老消息可能没有 MIME，按扩展名兜底。
  const isHtml =
    attachment?.mimeType === 'text/html' || lowerName.endsWith('.html') || lowerName.endsWith('.htm')
  const isMarkdown =
    attachment?.mimeType === 'text/markdown' ||
    lowerName.endsWith('.md') ||
    lowerName.endsWith('.markdown')
  // 收到的视频按文件投递（下载到缓存后把本地路径交给 AICLI），所以到这里是 file 类型。
  // 但卡片上得写「视频」——一段 mp4 标成「文件」会让人以为发错了。
  const isVideo =
    attachment?.mimeType?.toLowerCase().startsWith('video/') ||
    lowerName.endsWith('.mp4') ||
    lowerName.endsWith('.mov')
  const canPreview = Boolean(attachment?.localPath && (isHtml || isMarkdown))
  const typeLabel = isHtml
    ? 'HTML 文件'
    : isMarkdown
      ? 'Markdown 文件'
      : isVideo
        ? '视频'
        : '文件'

  return (
    <button
      type="button"
      className="remote-im-file-message"
      disabled={!canPreview}
      onClick={() => props.onPreview(props.message)}
      title={canPreview ? '点击预览文件' : '文件暂不可预览'}
    >
      <span className="remote-im-file-icon">{isVideo ? '视' : '文'}</span>
      <span className="remote-im-file-info">
        <strong>{fileName || '文件消息'}</strong>
        <em>{typeLabel}</em>
      </span>
    </button>
  )
}

export function formatRemoteImVideoDuration(seconds: number | null | undefined): string | null {
  if (typeof seconds !== 'number' || !Number.isFinite(seconds) || seconds <= 0) return null
  const total = Math.round(seconds)
  const minutes = Math.floor(total / 60)
  const rest = total % 60
  return `${minutes}:${String(rest).padStart(2, '0')}`
}

export function formatRemoteImVideoSize(sizeBytes: number | null | undefined): string | null {
  if (typeof sizeBytes !== 'number' || !Number.isFinite(sizeBytes) || sizeBytes <= 0) return null
  const mb = sizeBytes / (1024 * 1024)
  if (mb >= 1) return `${mb.toFixed(1)} MB`
  return `${Math.max(1, Math.round(sizeBytes / 1024))} KB`
}

function RemoteImVideoMessage(props: { message: RemoteImMessage }): JSX.Element {
  const attachment = props.message.attachment?.type === 'video' ? props.message.attachment : null
  const fileName = attachment?.fileName ?? props.message.content.replace(/^\[视频消息\]\s*/, '')
  // 封面由 IM 服务端在上传后生成，出站时本端拿不到，所以这里只在对端消息上才有图。
  const posterSource = getRemoteImImageSource(attachment?.thumbnailUrl)
  const meta = [
    formatRemoteImVideoDuration(attachment?.durationSeconds),
    formatRemoteImVideoSize(attachment?.sizeBytes)
  ].filter((part): part is string => Boolean(part))

  return (
    <div className="remote-im-video-message">
      <div className="remote-im-video-poster">
        {posterSource ? (
          <img src={posterSource} alt={fileName || '视频消息'} loading="lazy" />
        ) : null}
        <span className="remote-im-video-play" aria-hidden="true">
          ▶
        </span>
      </div>
      <div className="remote-im-video-info">
        <strong>{fileName || '视频消息'}</strong>
        <em>{meta.length > 0 ? `视频 · ${meta.join(' · ')}` : '视频'}</em>
      </div>
    </div>
  )
}

export function sanitizeRemoteImDisplayText(text: string): string {
  return text
    .replace(/Tencent IM/g, 'IM')
    .replace(/SDKAppID/g, 'IM 应用配置')
    .replace(/UserSig endpoint/gi, '凭证接口')
    .replace(/UserSig|usersig/gi, '登录凭证')
    .replace(/SecretKey/g, '连接凭证')
    .replace(/\bIM login failed\b/g, 'IM 登录失败')
    .replace(/\bIM send failed\b/g, 'IM 发送失败')
    .replace(/\bIM runtime is not connected\b/g, 'IM 运行时未连接')
    .replace(/\bIM send timed out\b/g, 'IM 发送超时')
    .replace(/invalid 登录凭证/g, '登录凭证无效')
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

export function scrollRemoteImMessagesToLatest(
  container: Pick<HTMLDivElement, 'scrollHeight' | 'scrollTop'>
): void {
  container.scrollTop = container.scrollHeight
}

export function shouldScrollRemoteImConversationToLatest(
  pendingPeerUserId: string | null,
  selectedPeerUserId: string | null,
  messageCount: number
): boolean {
  return Boolean(
    selectedPeerUserId &&
      pendingPeerUserId === selectedPeerUserId &&
      messageCount > 0
  )
}

function isInteractiveDragTarget(target: EventTarget | null): boolean {
  return target instanceof HTMLElement && Boolean(target.closest('button, input, select, textarea, a'))
}

export default function RemoteImDrawer(props: RemoteImDrawerProps): JSX.Element | null {
  const [conversationFilter, setConversationFilter] = useState<ConversationFilter>('recent')
  const [newContactUserId, setNewContactUserId] = useState('')
  const [loadingEarlier, setLoadingEarlier] = useState(false)
  const panelRef = useRef<HTMLDivElement | null>(null)
  const messagesRef = useRef<HTMLDivElement | null>(null)
  const previousMessageSelectionRef = useRef<string | null>(null)
  const pendingLatestScrollPeerRef = useRef<string | null>(null)
  const imageInputRef = useRef<HTMLInputElement | null>(null)
  const [panelPosition, setPanelPosition] = useState<RemoteImPanelPosition | null>(null)
  const [dragState, setDragState] = useState<{
    startPosition: RemoteImPanelPosition
    startPointer: RemoteImPanelPosition
  } | null>(null)
  const [filePreview, setFilePreview] = useState<FilePreviewState | null>(null)

  const conversations = useMemo(
    () => getRemoteImConversations(props.config, props.messages),
    [props.config, props.messages]
  )
  const filteredConversations =
    conversationFilter === 'recent'
      ? conversations
      // 「好友」页只列真正在联系人配置里的人。陌生人只在「最近」里出现——
      // 它们的消息本来就会被授权判定拒掉，混进好友页会让人以为已授权。
      : conversations.filter((conversation) => conversation.isContact)
  const selectedPeerUserId =
    props.selectedPeerUserId ?? filteredConversations[0]?.userId ?? conversations[0]?.userId ?? null
  const selectedMessages = selectedPeerUserId
    ? filterRemoteImMessagesByPeer(props.messages, props.config.desktopUserId, selectedPeerUserId)
    : []
  const selectedConversation = selectedPeerUserId
    ? conversations.find((conversation) => conversation.userId === selectedPeerUserId)
    : null
  const selectedLatestMessageId = selectedMessages.at(-1)?.id ?? null
  const inputDisabled =
    !selectedPeerUserId || !props.projectId || props.status?.state !== 'connected'
  const sendDisabled =
    isRemoteImSendDisabled({
      projectId: props.projectId,
      sessionRunning: props.sessionRunning,
      text: props.input,
      status: props.status
    }) || !selectedPeerUserId
  const imageSendDisabled =
    !selectedPeerUserId || !props.projectId || props.status?.state !== 'connected'
  const statusDetail = props.status?.detail
    ? sanitizeRemoteImDisplayText(props.status.detail)
    : null
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
    const selection = props.open ? selectedPeerUserId : null
    if (selection !== previousMessageSelectionRef.current) {
      previousMessageSelectionRef.current = selection
      pendingLatestScrollPeerRef.current = selection
    }
    if (!shouldScrollRemoteImConversationToLatest(
      pendingLatestScrollPeerRef.current,
      selection,
      selectedMessages.length
    )) {
      return
    }

    const frame = requestAnimationFrame(() => {
      if (pendingLatestScrollPeerRef.current !== selection) return
      const container = messagesRef.current
      if (!container) return
      scrollRemoteImMessagesToLatest(container)
      pendingLatestScrollPeerRef.current = null
    })
    return () => cancelAnimationFrame(frame)
  }, [props.open, selectedPeerUserId, selectedMessages.length, selectedLatestMessageId])

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

  function handleChooseImage(): void {
    if (imageSendDisabled) return
    imageInputRef.current?.click()
  }

  function handleSelectConversation(userId: string): void {
    pendingLatestScrollPeerRef.current = userId
    props.onSelectPeer(userId)
    if (!shouldScrollRemoteImConversationToLatest(
      pendingLatestScrollPeerRef.current,
      selectedPeerUserId,
      selectedMessages.length
    )) return
    requestAnimationFrame(() => {
      if (pendingLatestScrollPeerRef.current !== userId) return
      const container = messagesRef.current
      if (!container) return
      scrollRemoteImMessagesToLatest(container)
      pendingLatestScrollPeerRef.current = null
    })
  }

  function handleImageInputChange(event: ChangeEvent<HTMLInputElement>): void {
    const file = event.currentTarget.files?.[0]
    event.currentTarget.value = ''
    if (!file || !selectedPeerUserId || imageSendDisabled) return
    if (!file.type.startsWith('image/')) return
    props.onSendImage(selectedPeerUserId, file)
  }

  async function handlePreviewFile(message: RemoteImMessage): Promise<void> {
    const attachment = message.attachment?.type === 'file' ? message.attachment : null
    const fileName = (attachment?.fileName ?? message.content.replace(/^\[文件消息\]\s*/, '')) || '文件预览'
    setFilePreview({
      status: 'loading',
      fileName,
      mimeType: attachment?.mimeType ?? null,
      content: '',
      error: null
    })
    const result = await window.api.remoteIm.readFilePreview({
      localPath: attachment?.localPath ?? null,
      mimeType: attachment?.mimeType ?? null
    })
    if (!result.ok) {
      setFilePreview({
        status: 'error',
        fileName,
        mimeType: attachment?.mimeType ?? null,
        content: '',
        error: sanitizeRemoteImDisplayText(result.error)
      })
      return
    }
    setFilePreview({
      status: 'ready',
      fileName: result.value.fileName || fileName,
      mimeType: result.value.mimeType,
      content: result.value.content,
      error: null
    })
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
          <div
            className={`remote-im-status status-${props.status?.state ?? 'disconnected'}`}
            title={statusDetail ?? undefined}
          >
            <span />
            {getRemoteImStatusLabel(props.status)}
            {statusDetail ? (
              <small className="remote-im-status-detail">{statusDetail}</small>
            ) : null}
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

        <div className="remote-im-shell">
          <section className="remote-im-sidebar" aria-label="会话">
            <div className="remote-im-sidebar-head">
              <span>{props.config.desktopUserId || '未设置账号'}</span>
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
                      onClick={() => handleSelectConversation(conversation.userId)}
                    >
                      <div>
                        <strong>{conversation.userId}</strong>
                        <span>{conversation.lastMessagePreview || '暂无消息'}</span>
                      </div>
                      <em>{getRelationLabel(conversation.isContact)}</em>
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
                placeholder="账号 ID"
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
              {selectedConversation ? <em>{getRelationLabel(selectedConversation.isContact)}</em> : null}
            </div>

            <div ref={messagesRef} className="remote-im-messages">
              {selectedPeerUserId && selectedMessages.length > 0 && props.canLoadEarlier ? (
                <div className="remote-im-load-earlier-row">
                  <button
                    type="button"
                    className="remote-im-load-earlier"
                    disabled={loadingEarlier}
                    onClick={() => {
                      if (!selectedPeerUserId || loadingEarlier) return
                      setLoadingEarlier(true)
                      void Promise.resolve(props.onLoadEarlier?.(selectedPeerUserId)).finally(() =>
                        setLoadingEarlier(false)
                      )
                    }}
                  >
                    {loadingEarlier ? '加载中…' : '加载更早的消息'}
                  </button>
                </div>
              ) : null}
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
                            {getRelationLabel(displayMeta.isContact)}
                          </em>
                          <span>{formatRemoteImTime(message.createdAt)}</span>
                        </div>
                        <div className="remote-im-bubble">
                          {message.kind === 'image' ? (
                            <RemoteImImageMessage message={message} />
                          ) : message.kind === 'file' ? (
                            <RemoteImFileMessage message={message} onPreview={handlePreviewFile} />
                          ) : message.kind === 'video' ? (
                            <RemoteImVideoMessage message={message} />
                          ) : (
                            <RemoteImMarkdown content={message.content} />
                          )}
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
              <button
                type="button"
                className="remote-im-image-button"
                aria-label="发送图片"
                title="发送图片"
                disabled={imageSendDisabled}
                onClick={handleChooseImage}
              >
                图
              </button>
              <input
                ref={imageInputRef}
                className="remote-im-image-input"
                type="file"
                accept="image/jpeg,image/png,image/gif,image/webp"
                aria-label="选择图片"
                disabled={imageSendDisabled}
                onChange={handleImageInputChange}
              />
              <input
                value={props.input}
                onChange={(event) => props.onInputChange(event.currentTarget.value)}
                disabled={inputDisabled}
                placeholder={
                  selectedPeerUserId
                    ? '输入要发送给当前联系人的消息...'
                    : '先选择一个联系人'
                }
              />
              <button type="submit" disabled={sendDisabled}>
                发送
              </button>
            </form>
          </section>
        </div>
        {filePreview ? (
          <div className="remote-im-file-preview-backdrop" role="dialog" aria-modal="true">
            <div className="remote-im-file-preview-modal">
              <header>
                <strong>{filePreview.fileName}</strong>
                <button
                  type="button"
                  aria-label="关闭文件预览"
                  onClick={() => setFilePreview(null)}
                >
                  ×
                </button>
              </header>
              <div className="remote-im-file-preview-body">
                {filePreview.status === 'loading' ? (
                  <div className="remote-im-file-preview-empty">正在加载...</div>
                ) : filePreview.status === 'error' ? (
                  <div className="remote-im-file-preview-empty">{filePreview.error}</div>
                ) : filePreview.mimeType === 'text/html' ? (
                  <iframe
                    title={filePreview.fileName}
                    sandbox=""
                    srcDoc={filePreview.content}
                  />
                ) : (
                  <div className="remote-im-file-preview-markdown">
                    <RemoteImMarkdown content={filePreview.content} />
                  </div>
                )}
              </div>
            </div>
          </div>
        ) : null}
      </div>
    </aside>
  )
}
