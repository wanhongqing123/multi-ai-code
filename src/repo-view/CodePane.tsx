import { useCallback, useEffect, useMemo, useRef, useState } from 'react'

export interface RepoSelection {
  snippet: string
  lineRange: string
}

export interface CodePaneProps {
  filePath: string
  content: string
  byteLength: number
  loading: boolean
  onAnnotateSelection: (selection: RepoSelection, comment: string, editingId?: string) => void
  editingAnnotation?: {
    id: string
    lineRange: string
    snippet: string
    comment: string
  } | null
  onCancelEditing: () => void
}

function lineFromNode(node: Node | null): number | null {
  if (!node) return null
  const el = node instanceof Element ? node : node.parentElement
  const lineEl = el?.closest<HTMLElement>('.repo-code-line')
  const raw = lineEl?.dataset['line']
  if (!raw) return null
  const n = Number(raw)
  return Number.isFinite(n) ? n : null
}

export default function CodePane({
  filePath,
  content,
  byteLength,
  loading,
  onAnnotateSelection,
  editingAnnotation,
  onCancelEditing
}: CodePaneProps): JSX.Element {
  const paneRef = useRef<HTMLDivElement | null>(null)
  const [draft, setDraft] = useState<{
    quote: string
    lineRange: string
    x: number
    y: number
  } | null>(null)
  const [composer, setComposer] = useState<{
    snippet: string
    lineRange: string
    comment: string
    editingId?: string
  } | null>(null)

  const lineCount = useMemo(() => (content ? content.split('\n').length : 0), [content])

  const handleMouseUp = useCallback(() => {
    const sel = window.getSelection()
    if (!sel || sel.isCollapsed || !paneRef.current) {
      setDraft(null)
      return
    }
    const text = sel.toString().trim()
    if (!text) {
      setDraft(null)
      return
    }
    const range = sel.getRangeAt(0)
    if (!paneRef.current.contains(range.commonAncestorContainer)) {
      setDraft(null)
      return
    }
    const rect = range.getBoundingClientRect()
    const paneRect = paneRef.current.getBoundingClientRect()
    const start = lineFromNode(range.startContainer)
    const end = lineFromNode(range.endContainer)
    const lo = typeof start === 'number' && typeof end === 'number' ? Math.min(start, end) : null
    const hi = typeof start === 'number' && typeof end === 'number' ? Math.max(start, end) : null
    const lineRange = lo && hi ? (lo === hi ? `${lo}` : `${lo}-${hi}`) : '未知行'
    setDraft({
      quote: text,
      lineRange,
      x: rect.right - paneRect.left + 6,
      y: rect.top - paneRect.top + paneRef.current.scrollTop - 6
    })
  }, [])

  const addAnnotation = useCallback(() => {
    if (!draft) return
    setComposer({
      snippet: draft.quote,
      lineRange: draft.lineRange,
      comment: ''
    })
    setDraft(null)
  }, [draft])

  const cancelComposer = useCallback(() => {
    setComposer(null)
    onCancelEditing()
  }, [onCancelEditing])

  const saveComposer = useCallback(() => {
    if (!composer) return
    const comment = composer.comment.trim()
    if (!comment) return
    onAnnotateSelection(
      { snippet: composer.snippet, lineRange: composer.lineRange },
      comment,
      composer.editingId
    )
    setComposer(null)
    onCancelEditing()
    const sel = window.getSelection()
    sel?.removeAllRanges()
  }, [composer, onAnnotateSelection, onCancelEditing])

  useEffect(() => {
    if (!editingAnnotation) return
    setComposer({
      snippet: editingAnnotation.snippet,
      lineRange: editingAnnotation.lineRange,
      comment: editingAnnotation.comment,
      editingId: editingAnnotation.id
    })
    setDraft(null)
  }, [editingAnnotation])

  return (
    <div className="repo-code-wrap">
      <div className="repo-code-head" title={filePath || '未选择文件'}>
        <span>{filePath || '未选择文件'}</span>
        {filePath && (
          <span className="repo-code-meta">
            {lineCount} 行 · {byteLength} bytes
          </span>
        )}
      </div>
      <div
        className="repo-code-pane"
        ref={paneRef}
        onMouseUp={handleMouseUp}
        onScroll={() => setDraft(null)}
      >
        {!filePath ? (
          <div className="repo-code-empty">从左侧选择一个文件以查看源码</div>
        ) : loading ? (
          <div className="repo-code-empty">读取中…</div>
        ) : (
          <>
            <pre className="repo-code-pre">
              {content.split('\n').map((line, index) => (
                <div key={index} className="repo-code-line" data-line={index + 1}>
                  <span className="repo-code-gutter">{index + 1}</span>
                  <span className="repo-code-text">{line || ' '}</span>
                </div>
              ))}
            </pre>
            {draft && (
              <button
                className="repo-code-annotate-floater"
                style={{ left: draft.x, top: draft.y }}
                onClick={addAnnotation}
                title="把选中代码加入分析批注"
              >
                ✏ 标注
              </button>
            )}
          </>
        )}
      </div>
      {composer && (
        <div className="plan-review-composer-backdrop" onClick={cancelComposer}>
          <div
            className="plan-review-composer"
            onClick={(e) => e.stopPropagation()}
          >
            <div className="plan-review-composer-head">
              {composer.editingId ? '编辑批注' : '添加批注'} · {filePath}:{composer.lineRange}
            </div>
            <blockquote className="plan-review-composer-quote">
              {composer.snippet.length > 500
                ? composer.snippet.slice(0, 500) + '…'
                : composer.snippet}
            </blockquote>
            <textarea
              className="plan-review-composer-input"
              value={composer.comment}
              onChange={(e) =>
                setComposer({ ...composer, comment: e.target.value })
              }
              autoFocus
              placeholder="写下这段代码需要分析或修改的具体说明..."
              rows={5}
            />
            <div className="plan-review-composer-actions">
              <button className="drawer-btn" onClick={cancelComposer}>
                取消
              </button>
              <span style={{ flex: 1 }} />
              <button
                className="drawer-btn primary"
                onClick={saveComposer}
                disabled={!composer.comment.trim()}
              >
                {composer.editingId ? '保存修改' : '添加批注'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
