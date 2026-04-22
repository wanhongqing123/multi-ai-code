import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import { getCliTargetLabel, type AiCliKind } from './cliTarget'

export interface Annotation {
  id: string
  quote: string
  comment: string
}

export interface PlanReviewDialogProps {
  path: string
  content: string
  title?: string
  aiCli?: AiCliKind
  /** Called when user submits annotations back to the CLI. */
  onSubmit: (annotations: Annotation[], generalNote: string) => Promise<void> | void
  onClose: () => void
}

function genId(): string {
  return `a_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`
}

export default function PlanReviewDialog({
  path,
  content,
  title,
  aiCli = 'claude',
  onSubmit,
  onClose
}: PlanReviewDialogProps) {
  const [annotations, setAnnotations] = useState<Annotation[]>([])
  const [generalNote, setGeneralNote] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [draft, setDraft] = useState<{
    quote: string
    x: number
    y: number
  } | null>(null)
  const [composerQuote, setComposerQuote] = useState<string | null>(null)
  const [composerComment, setComposerComment] = useState('')
  const [editingId, setEditingId] = useState<string | null>(null)

  const mdRef = useRef<HTMLDivElement | null>(null)
  const cliTargetLabel = useMemo(() => getCliTargetLabel(aiCli), [aiCli])

  const filename = useMemo(() => path.split(/[\\/]/).pop() ?? path, [path])

  // Capture text selection inside the markdown pane → show floating "✏ 标注" button.
  const handleMouseUp = useCallback(() => {
    const sel = window.getSelection()
    if (!sel || sel.isCollapsed) {
      setDraft(null)
      return
    }
    const text = sel.toString().trim()
    if (!text || !mdRef.current) {
      setDraft(null)
      return
    }
    const range = sel.getRangeAt(0)
    // Only accept selection that lives inside the markdown pane.
    if (!mdRef.current.contains(range.commonAncestorContainer)) {
      setDraft(null)
      return
    }
    const rect = range.getBoundingClientRect()
    const paneRect = mdRef.current.getBoundingClientRect()
    setDraft({
      quote: text,
      x: rect.right - paneRect.left + 4,
      y: rect.top - paneRect.top + mdRef.current.scrollTop - 4
    })
  }, [])

  // Dismiss the floating button on outside click / escape.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (composerQuote !== null) {
          setComposerQuote(null)
          setComposerComment('')
          setEditingId(null)
        } else if (draft) {
          setDraft(null)
        } else {
          onClose()
        }
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [draft, composerQuote, onClose])

  const openComposerFor = useCallback(
    (quote: string, editId: string | null = null, comment = '') => {
      setComposerQuote(quote)
      setComposerComment(comment)
      setEditingId(editId)
      setDraft(null)
    },
    []
  )

  const saveAnnotation = useCallback(() => {
    if (!composerQuote) return
    const comment = composerComment.trim()
    if (!comment) return
    if (editingId) {
      setAnnotations((prev) =>
        prev.map((a) => (a.id === editingId ? { ...a, comment } : a))
      )
    } else {
      setAnnotations((prev) => [
        ...prev,
        { id: genId(), quote: composerQuote, comment }
      ])
    }
    setComposerQuote(null)
    setComposerComment('')
    setEditingId(null)
  }, [composerQuote, composerComment, editingId])

  const cancelComposer = useCallback(() => {
    setComposerQuote(null)
    setComposerComment('')
    setEditingId(null)
  }, [])

  const removeAnnotation = useCallback((id: string) => {
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
  }, [])

  const editAnnotation = useCallback((a: Annotation) => {
    openComposerFor(a.quote, a.id, a.comment)
  }, [openComposerFor])

  const handleSubmit = useCallback(async () => {
    if (annotations.length === 0 && !generalNote.trim()) return
    setSubmitting(true)
    try {
      await onSubmit(annotations, generalNote.trim())
    } finally {
      setSubmitting(false)
    }
  }, [annotations, generalNote, onSubmit])

  const canSubmit =
    !submitting && (annotations.length > 0 || generalNote.trim().length > 0)

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal plan-review-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-head">
          <h3>{title ?? `方案预览 · 可标注后反馈给 ${cliTargetLabel}`}</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="plan-review-meta" title={path}>
          <span className="file-preview-name">📄 {filename}</span>
          <span className="file-preview-sep">·</span>
          <span>{annotations.length} 条标注</span>
          <span className="file-preview-path">{path}</span>
        </div>

        <div className="plan-review-body">
          <div
            className="plan-review-pane md-rendered"
            ref={mdRef}
            onMouseUp={handleMouseUp}
          >
            <ReactMarkdown
              remarkPlugins={[remarkGfm]}
              components={{
                a: ({ href, children, ...rest }) => {
                  const safe =
                    typeof href === 'string' && /^(https?:|mailto:|#)/i.test(href)
                  return safe ? (
                    <a href={href} target="_blank" rel="noopener noreferrer" {...rest}>
                      {children}
                    </a>
                  ) : (
                    <span>{children}</span>
                  )
                },
                img: ({ src, alt }) => {
                  const safe =
                    typeof src === 'string' && /^(https?:|data:image\/)/i.test(src)
                  return safe ? (
                    <img src={src} alt={alt ?? ''} />
                  ) : (
                    <span>[image: {alt}]</span>
                  )
                }
              }}
            >
              {content}
            </ReactMarkdown>

            {draft && (
              <button
                className="plan-review-annotate-floater"
                style={{ left: draft.x, top: draft.y }}
                onClick={() => openComposerFor(draft.quote)}
                title="为这段选区添加标注"
              >
                ✏ 标注
              </button>
            )}
          </div>

          <aside className="plan-review-side">
            <div className="plan-review-side-title">批注列表</div>
            {annotations.length === 0 ? (
              <div className="plan-review-empty">
                在左侧选中文字 → 点浮现的 "✏ 标注" 按钮添加。
                <br />
                也可在下方"整体意见"里写不针对具体段落的反馈。
              </div>
            ) : (
              <ul className="plan-review-list">
                {annotations.map((a, i) => (
                  <li key={a.id} className="plan-review-item">
                    <div className="plan-review-item-head">
                      <span className="plan-review-item-idx">#{i + 1}</span>
                      <div className="plan-review-item-actions">
                        <button
                          className="plan-review-item-btn"
                          onClick={() => editAnnotation(a)}
                          title="编辑批注"
                        >
                          ✎
                        </button>
                        <button
                          className="plan-review-item-btn danger"
                          onClick={() => removeAnnotation(a.id)}
                          title="删除"
                        >
                          ✕
                        </button>
                      </div>
                    </div>
                    <blockquote className="plan-review-item-quote">
                      {a.quote.length > 240 ? a.quote.slice(0, 240) + '…' : a.quote}
                    </blockquote>
                    <div className="plan-review-item-comment">{a.comment}</div>
                  </li>
                ))}
              </ul>
            )}

            <div className="plan-review-general">
              <label className="plan-review-general-label">整体意见（可选）</label>
              <textarea
                className="plan-review-general-input"
                value={generalNote}
                onChange={(e) => setGeneralNote(e.target.value)}
                placeholder="对方案的整体看法、新增要求、范围调整等..."
                rows={3}
              />
            </div>
          </aside>
        </div>

        {composerQuote !== null && (
          <div className="plan-review-composer-backdrop" onClick={cancelComposer}>
            <div
              className="plan-review-composer"
              onClick={(e) => e.stopPropagation()}
            >
              <div className="plan-review-composer-head">
                {editingId ? '编辑批注' : '添加批注'}
              </div>
              <blockquote className="plan-review-composer-quote">
                {composerQuote.length > 400
                  ? composerQuote.slice(0, 400) + '…'
                  : composerQuote}
              </blockquote>
              <textarea
                className="plan-review-composer-input"
                value={composerComment}
                onChange={(e) => setComposerComment(e.target.value)}
                autoFocus
                placeholder="写下你对这段的意见，例如：这里没考虑 XXX 的情况 / 需要补充 YYY 的接口..."
                rows={4}
              />
              <div className="plan-review-composer-actions">
                <button className="drawer-btn" onClick={cancelComposer}>
                  取消
                </button>
                <span style={{ flex: 1 }} />
                <button
                  className="drawer-btn primary"
                  onClick={saveAnnotation}
                  disabled={!composerComment.trim()}
                >
                  {editingId ? '保存修改' : '添加批注'}
                </button>
              </div>
            </div>
          </div>
        )}

        <div className="drawer-actions plan-review-actions">
          <button className="drawer-btn" onClick={onClose} disabled={submitting}>
            ✕ 关闭（不发送）
          </button>
          <span style={{ flex: 1 }} />
          <button
            className="drawer-btn primary"
            onClick={handleSubmit}
            disabled={!canSubmit}
            title={
              canSubmit
                ? `把批注合成一条消息发送给 ${cliTargetLabel}，让 CLI 根据意见修改方案`
                : '至少添加一条批注或整体意见'
            }
          >
            {submitting
              ? '发送中…'
              : `📤 发送给 ${cliTargetLabel}（${annotations.length} 条批注${generalNote.trim() ? ' + 整体意见' : ''}）`}
          </button>
        </div>
      </div>
    </div>
  )
}
