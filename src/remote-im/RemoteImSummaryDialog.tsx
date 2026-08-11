import { Fragment, useEffect, useMemo, useRef, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { RemoteImMessage } from '../../electron/preload.js'
import {
  buildRemoteImMessageSummaryMarkdown,
  formatSummaryDay,
  formatSummaryTime,
  summarizeRemoteImMessages,
  summaryAttachmentParts,
  summaryMessageContent,
  summarySenderLabel
} from './messageSummary.js'

type RemoteImSummaryImageSource =
  | { status: 'loading' }
  | { status: 'ready'; url: string }
  | { status: 'error'; message: string }

function remoteImageFallback(message: RemoteImMessage): string | null {
  const attachment = message.attachment?.type === 'image' ? message.attachment : null
  const source = attachment?.thumbnailUrl?.trim() || attachment?.remoteUrl?.trim()
  return source && /^(https?:|data:image\/)/i.test(source) ? source : null
}

export function RemoteImSummaryImage(props: {
  projectId: string
  message: RemoteImMessage
}): JSX.Element {
  const attachment = props.message.attachment?.type === 'image' ? props.message.attachment : null
  const fallback = remoteImageFallback(props.message)
  const containerRef = useRef<HTMLDivElement | null>(null)
  const [shouldLoadLocal, setShouldLoadLocal] = useState(false)
  const [source, setSource] = useState<RemoteImSummaryImageSource>(() =>
    attachment?.localPath
      ? { status: 'loading' }
      : fallback
        ? { status: 'ready', url: fallback }
        : { status: 'error', message: '图片暂不可预览' }
  )

  useEffect(() => {
    if (!attachment?.localPath) {
      setShouldLoadLocal(false)
      return
    }

    const element = containerRef.current
    if (typeof window === 'undefined' || !element || typeof IntersectionObserver === 'undefined') {
      setShouldLoadLocal(true)
      return
    }

    setShouldLoadLocal(false)
    const observer = new IntersectionObserver(
      (entries) => {
        if (!entries.some((entry) => entry.isIntersecting)) return
        setShouldLoadLocal(true)
        observer.disconnect()
      },
      { rootMargin: '400px 0px' }
    )
    observer.observe(element)
    return () => observer.disconnect()
  }, [attachment?.localPath, props.message.id])

  useEffect(() => {
    if (!attachment?.localPath) {
      setSource(
        fallback
          ? { status: 'ready', url: fallback }
          : { status: 'error', message: '图片暂不可预览' }
      )
      return
    }
    if (!shouldLoadLocal) return

    let cancelled = false
    setSource({ status: 'loading' })
    const readImage =
      typeof window === 'undefined' ? undefined : window.api?.remoteIm?.readImagePreview
    if (!readImage) {
      setSource(
        fallback
          ? { status: 'ready', url: fallback }
          : { status: 'error', message: '图片读取接口不可用' }
      )
      return
    }

    void readImage({ projectId: props.projectId, messageId: props.message.id })
      .then((result) => {
        if (cancelled) return
        setSource(
          result.ok
            ? { status: 'ready', url: result.dataUrl }
            : fallback
              ? { status: 'ready', url: fallback }
              : { status: 'error', message: result.error }
        )
      })
      .catch((error: unknown) => {
        if (cancelled) return
        setSource(
          fallback
            ? { status: 'ready', url: fallback }
            : {
                status: 'error',
                message: error instanceof Error ? error.message : '图片读取失败'
              }
        )
      })

    return () => {
      cancelled = true
    }
  }, [attachment?.localPath, fallback, props.message.id, props.projectId, shouldLoadLocal])

  const label = attachment?.fileName ?? '图片消息'
  return (
    <div ref={containerRef} className="remote-im-summary-image">
      {source.status === 'ready' ? (
        <img
          className="remote-im-summary-image-preview"
          src={source.url}
          alt={label}
          loading="lazy"
          onError={() => {
            setSource((current) =>
              fallback && current.status === 'ready' && current.url !== fallback
                ? { status: 'ready', url: fallback }
                : { status: 'error', message: '图片暂不可预览' }
            )
          }}
        />
      ) : (
        <div className={`remote-im-summary-image-state ${source.status}`}>
          {source.status === 'loading' ? `正在读取 ${label}` : source.message}
        </div>
      )}
    </div>
  )
}

export interface RemoteImSummaryDialogProps {
  open: boolean
  projectId: string | null
  ownerUserId?: string | null
  // 主会话运行中才能发送；处理端把汇总落成 .md 文件并交给当前 AICLI 读取。
  canSendToAicli: boolean
  onSendToAicli?: (markdown: string) => Promise<boolean>
  onClose: () => void
}

// 消息记录汇总弹窗：结构化文档视图（统计徽章、会话卡片、日期分隔、方向着色的
// 发送者胶囊），消息正文仍用 Markdown 渲染（AICLI 输出的标题/列表/代码块不丢）。
// 「发送给 AICLI」用共享生成器落成完整 .md 文件后把路径交给主会话。
export default function RemoteImSummaryDialog(props: RemoteImSummaryDialogProps): JSX.Element | null {
  const [messages, setMessages] = useState<RemoteImMessage[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [sending, setSending] = useState(false)

  useEffect(() => {
    if (!props.open || !props.projectId) return
    let cancelled = false
    setMessages(null)
    setError(null)
    setSending(false)
    window.api.remoteIm
      .listMessagesForSummary(props.projectId)
      .then((list) => {
        if (!cancelled) setMessages(list)
      })
      .catch((cause: unknown) => {
        if (!cancelled) setError(cause instanceof Error ? cause.message : '读取消息记录失败')
      })
    return () => {
      cancelled = true
    }
  }, [props.open, props.projectId])

  const summary = useMemo(() => (messages ? summarizeRemoteImMessages(messages) : null), [messages])
  const markdown = useMemo(
    () =>
      messages
        ? buildRemoteImMessageSummaryMarkdown(messages, { ownerUserId: props.ownerUserId })
        : '',
    [messages, props.ownerUserId]
  )

  if (!props.open) return null

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div className="modal remote-im-summary-modal" onClick={(event) => event.stopPropagation()}>
        <header className="remote-im-summary-header">
          <div className="remote-im-summary-actions">
            <button
              type="button"
              className="remote-im-summary-send"
              disabled={!markdown || sending || !props.canSendToAicli || !props.onSendToAicli}
              title={
                props.canSendToAicli
                  ? '开启时光胶囊：把全部消息记录交给当前 AICLI，帮它找回此前的对话记忆与背景'
                  : '主会话未运行，先启动 AICLI 会话'
              }
              onClick={() => {
                if (!props.onSendToAicli || !markdown) return
                setSending(true)
                // 结果反馈交给 toast，按钮文案保持不变；sending 仅用于防连点。
                void props.onSendToAicli(markdown).finally(() => setSending(false))
              }}
            >
              ⏳ 时光胶囊
            </button>
            <button type="button" className="remote-im-close" onClick={props.onClose}>
              ×
            </button>
          </div>
        </header>
        <div className="remote-im-summary-body">
          {error ? (
            <div className="remote-im-summary-error">{error}</div>
          ) : messages === null ? (
            <div className="remote-im-summary-loading">加载消息记录中…</div>
          ) : !summary ? (
            <div className="remote-im-summary-loading">暂无消息记录</div>
          ) : (
            <div className="remote-im-summary-doc">
              <div className="remote-im-summary-stats">
                <span className="remote-im-summary-stat">
                  <b>{summary.total}</b> 条消息
                </span>
                <span className="remote-im-summary-stat">
                  <b>{summary.sessionCount}</b> 个会话
                </span>
                <span className="remote-im-summary-stat remote-im-summary-stat-range">
                  {formatSummaryTime(summary.firstAt)} ~ {formatSummaryTime(summary.lastAt)}
                </span>
              </div>
              {summary.groups.map((group) => {
                let lastDay = ''
                return (
                  <section className="remote-im-summary-session" key={group.peer}>
                    <header className="remote-im-summary-session-head">
                      <span className="remote-im-summary-avatar" aria-hidden>
                        {(group.peer[0] ?? '#').toUpperCase()}
                      </span>
                      <span className="remote-im-summary-session-name">{group.peer}</span>
                      <span className="remote-im-summary-session-count">{group.messages.length} 条</span>
                    </header>
                    {group.messages.map((message) => {
                      const day = formatSummaryDay(message.createdAt)
                      const showDay = day !== lastDay
                      if (showDay) lastDay = day
                      const attachment = summaryAttachmentParts(message)
                      const content = summaryMessageContent(message)
                      return (
                        <Fragment key={message.id}>
                          {showDay ? (
                            <div className="remote-im-summary-day">
                              <span>{day}</span>
                            </div>
                          ) : null}
                          <div className="remote-im-summary-msg" data-direction={message.direction}>
                            <div className="remote-im-summary-msg-meta">
                              <span className="remote-im-summary-sender">
                                {summarySenderLabel(message, props.ownerUserId)}
                              </span>
                              <span className="remote-im-summary-clock">
                                {formatSummaryTime(message.createdAt)}
                              </span>
                              {message.status === 'failed' ? (
                                <span className="remote-im-summary-failed">⚠️ 发送失败</span>
                              ) : null}
                            </div>
                            {attachment ? (
                              <div className="remote-im-summary-attachment">
                                <span aria-hidden>{attachment.icon}</span>
                                <span>{attachment.kindLabel}</span>
                                {attachment.fileName ? <code>{attachment.fileName}</code> : null}
                              </div>
                            ) : null}
                            {message.kind === 'image' ? (
                              <RemoteImSummaryImage
                                projectId={props.projectId ?? message.projectId ?? ''}
                                message={message}
                              />
                            ) : null}
                            {content ? (
                              <div className="remote-im-summary-content remote-im-markdown">
                                <ReactMarkdown remarkPlugins={[remarkGfm]}>{content}</ReactMarkdown>
                              </div>
                            ) : null}
                          </div>
                        </Fragment>
                      )
                    })}
                  </section>
                )
              })}
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
