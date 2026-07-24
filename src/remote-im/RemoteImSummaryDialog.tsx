import { Fragment, useEffect, useMemo, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { RemoteImMessage } from '../../electron/preload.js'
import {
  buildRemoteImMessageSummaryMarkdown,
  formatSummaryClock,
  formatSummaryDay,
  formatSummaryTime,
  summarizeRemoteImMessages,
  summaryAttachmentParts,
  summarySenderLabel
} from './messageSummary.js'

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
  const [sent, setSent] = useState(false)

  useEffect(() => {
    if (!props.open || !props.projectId) return
    let cancelled = false
    setMessages(null)
    setError(null)
    setSending(false)
    setSent(false)
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

  useEffect(() => {
    if (!sent) return
    const timer = window.setTimeout(() => setSent(false), 2000)
    return () => window.clearTimeout(timer)
  }, [sent])

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
                  ? '把消息记录交给当前 AICLI 阅读，帮它找回此前的对话记忆与背景'
                  : '主会话未运行，先启动 AICLI 会话'
              }
              onClick={() => {
                if (!props.onSendToAicli || !markdown) return
                setSending(true)
                void props
                  .onSendToAicli(markdown)
                  .then((ok) => {
                    if (ok) setSent(true)
                  })
                  .finally(() => setSending(false))
              }}
            >
              {sending ? '正在找回…' : sent ? '记忆已找回' : '找回记忆'}
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
                      const content = message.content.trim()
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
                                {formatSummaryClock(message.createdAt)}
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
