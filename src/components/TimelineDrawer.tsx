import { useEffect, useState } from 'react'

export interface TimelineDrawerProps {
  projectId: string
  onClose: () => void
}

interface Evt {
  id: number
  project_id: string
  from_stage: number | null
  to_stage: number | null
  kind: string
  payload: string | null
  created_at: string
}

export default function TimelineDrawer({ projectId, onClose }: TimelineDrawerProps) {
  const [events, setEvents] = useState<Evt[]>([])

  useEffect(() => {
    window.api.events.list(projectId, 500).then(setEvents)
  }, [projectId])

  function renderKind(kind: string): { label: string; color: string } {
    if (kind === 'stage:done') return { label: '✓ 阶段完成', color: '#7fd67f' }
    if (kind === 'handoff') return { label: '→ Handoff', color: '#4aa3ff' }
    if (kind === 'feedback') return { label: '↺ 回退 Feedback', color: '#ffb74a' }
    if (kind.startsWith('artifact:')) return { label: '📦 ' + kind.slice(9), color: '#b87fff' }
    return { label: kind, color: '#aaa' }
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal timeline-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>📜 审计时间线 · {events.length}</h3>
          <button className="modal-close" onClick={onClose}>×</button>
        </div>
        <div className="timeline-body">
          {events.length === 0 ? (
            <div className="drawer-empty">暂无事件</div>
          ) : (
            events.map((e) => {
              let payload: any = null
              try {
                payload = e.payload ? JSON.parse(e.payload) : null
              } catch {
                /* ignore */
              }
              const { label, color } = renderKind(e.kind)
              return (
                <div key={e.id} className="timeline-row">
                  <span className="timeline-time">{new Date(e.created_at).toLocaleString()}</span>
                  <span className="timeline-kind" style={{ color }}>{label}</span>
                  <span className="timeline-payload">
                    {payload?.label && <code>[{payload.label}]</code>}{' '}
                    {payload?.summary && <span>{payload.summary}</span>}
                    {payload?.verdict && <span className="timeline-verdict"> verdict={payload.verdict}</span>}
                    {payload?.note && <span className="timeline-note">{payload.note.slice(0, 120)}</span>}
                  </span>
                </div>
              )
            })
          )}
        </div>
      </div>
    </div>
  )
}
