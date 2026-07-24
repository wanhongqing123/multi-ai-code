import { useEffect, useMemo, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { RemoteImMessage } from '../../electron/preload.js'
import { buildRemoteImMessageSummaryMarkdown } from './messageSummary.js'

export interface RemoteImSummaryDialogProps {
  open: boolean
  projectId: string | null
  ownerUserId?: string | null
  onClose: () => void
}

// 消息记录汇总弹窗：取回项目全部消息 → 生成一份 Markdown 汇总文档渲染展示，
// 支持一键复制原始 Markdown（便于贴到日报/文档里）。
export default function RemoteImSummaryDialog(props: RemoteImSummaryDialogProps): JSX.Element | null {
  const [messages, setMessages] = useState<RemoteImMessage[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [copied, setCopied] = useState(false)

  useEffect(() => {
    if (!props.open || !props.projectId) return
    let cancelled = false
    setMessages(null)
    setError(null)
    setCopied(false)
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

  const markdown = useMemo(
    () =>
      messages
        ? buildRemoteImMessageSummaryMarkdown(messages, { ownerUserId: props.ownerUserId })
        : '',
    [messages, props.ownerUserId]
  )

  useEffect(() => {
    if (!copied) return
    const timer = window.setTimeout(() => setCopied(false), 1500)
    return () => window.clearTimeout(timer)
  }, [copied])

  if (!props.open) return null

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div className="modal remote-im-summary-modal" onClick={(event) => event.stopPropagation()}>
        <header className="remote-im-summary-header">
          <h2>消息记录汇总</h2>
          <div className="remote-im-summary-actions">
            <button
              type="button"
              className="remote-im-summary-copy"
              disabled={!markdown}
              onClick={() => {
                void navigator.clipboard.writeText(markdown).then(() => setCopied(true))
              }}
            >
              {copied ? '已复制' : '复制 Markdown'}
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
          ) : (
            <div className="remote-im-markdown">
              <ReactMarkdown remarkPlugins={[remarkGfm]}>{markdown}</ReactMarkdown>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
