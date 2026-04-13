import { useMemo, useState } from 'react'
import type { ReviewItem } from '../utils/parseReviewItems'
import { parseReviewItems, buildSelectedFeedback } from '../utils/parseReviewItems'

export interface ReviewChecklistProps {
  artifactContent: string
  /** UI-facing target stage label (defaults to "3"). */
  targetStageLabel?: string | number
  /** Receives the constructed feedback markdown for the selected items. */
  onAccept: (feedback: string, count: number) => void | Promise<void>
  onDismiss: () => void
  busy?: boolean
}

export default function ReviewChecklist({
  artifactContent,
  targetStageLabel = 3,
  onAccept,
  onDismiss,
  busy
}: ReviewChecklistProps) {
  const items = useMemo(() => parseReviewItems(artifactContent), [artifactContent])

  // default: must-fix checked, suggestion unchecked
  const [selected, setSelected] = useState<Record<number, boolean>>(() => {
    const init: Record<number, boolean> = {}
    for (const it of items) init[it.id] = it.severity === 'must-fix'
    return init
  })
  const [expanded, setExpanded] = useState<Record<number, boolean>>({})

  const checkedCount = items.filter((i) => selected[i.id]).length

  function toggle(id: number) {
    setSelected((s) => ({ ...s, [id]: !s[id] }))
  }

  function setAll(value: boolean, only?: 'must-fix' | 'suggestion') {
    setSelected(() => {
      const next: Record<number, boolean> = {}
      for (const it of items) {
        next[it.id] = only ? (it.severity === only ? value : selected[it.id] ?? false) : value
      }
      return next
    })
  }

  async function handleAccept() {
    const chosen = items.filter((i) => selected[i.id])
    const feedback = buildSelectedFeedback(chosen)
    await onAccept(feedback, chosen.length)
  }

  if (items.length === 0) {
    return (
      <div className="review-empty">
        <p>无法从 review 产物中解析出条目（格式可能不符）。</p>
        <p>原始内容：</p>
        <pre className="drawer-artifact">{artifactContent || '(空)'}</pre>
        <div className="drawer-actions">
          <button className="drawer-btn secondary" onClick={onDismiss} disabled={busy}>
            稍后决定
          </button>
        </div>
      </div>
    )
  }

  return (
    <div className="review-checklist">
      <div className="review-toolbar">
        <span>共 {items.length} 项 · 已选 {checkedCount}</span>
        <div className="review-toolbar-actions">
          <button onClick={() => setAll(true)}>全选</button>
          <button onClick={() => setAll(false)}>全不选</button>
          <button onClick={() => setAll(true, 'must-fix')}>仅 must-fix</button>
        </div>
      </div>

      <ul className="review-list">
        {items.map((it) => (
          <li
            key={it.id}
            className={`review-item ${it.severity} ${selected[it.id] ? 'checked' : ''}`}
          >
            <label className="review-item-head">
              <input
                type="checkbox"
                checked={selected[it.id] ?? false}
                onChange={() => toggle(it.id)}
                disabled={busy}
              />
              <span className={`severity-badge ${it.severity}`}>{it.severity}</span>
              <span className="review-item-loc">{it.location}</span>
              <span className="review-item-title">{it.title}</span>
              <button
                className="review-item-toggle"
                type="button"
                onClick={(e) => {
                  e.preventDefault()
                  setExpanded((s) => ({ ...s, [it.id]: !s[it.id] }))
                }}
                title="展开/收起详情"
              >
                {expanded[it.id] ? '▾' : '▸'}
              </button>
            </label>
            {expanded[it.id] && <pre className="review-item-body">{it.body}</pre>}
          </li>
        ))}
      </ul>

      <div className="drawer-actions">
        <button className="drawer-btn secondary" onClick={onDismiss} disabled={busy}>
          全部驳回
        </button>
        <button
          className="drawer-btn warn"
          onClick={handleAccept}
          disabled={busy || checkedCount === 0}
        >
          {busy ? '注入中…' : `回退到 Stage ${targetStageLabel} 修复 (${checkedCount} 项)`}
        </button>
      </div>
    </div>
  )
}
