import { useEffect, useMemo, useState } from 'react'
import type { ArtifactRecord } from '../../electron/preload'

const STAGE_NAMES: Record<number, string> = {
  1: '方案设计',
  2: '方案实施',
  3: '方案验收',
  4: '测试验证'
}

export interface HistoryDrawerProps {
  projectId: string
  projectDir: string
  onClose: () => void
}

export default function HistoryDrawer({
  projectId,
  projectDir,
  onClose
}: HistoryDrawerProps) {
  const [records, setRecords] = useState<ArtifactRecord[]>([])
  const [selected, setSelected] = useState<ArtifactRecord | null>(null)
  const [preview, setPreview] = useState<string>('')
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    window.api.artifact.list(projectId).then(setRecords)
  }, [projectId])

  const byStage = useMemo(() => {
    const out: Record<number, ArtifactRecord[]> = { 1: [], 2: [], 3: [], 4: [] }
    for (const r of records) {
      if (out[r.stage_id]) out[r.stage_id].push(r)
    }
    return out
  }, [records])

  async function openRecord(r: ArtifactRecord) {
    setSelected(r)
    setLoading(true)
    setError(null)
    const res = await window.api.artifact.read(projectDir, r.path)
    setLoading(false)
    if (res.ok) setPreview(res.content ?? '')
    else setError(res.error ?? 'read failed')
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal history-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-head">
          <h3>📋 产物历史 · {records.length} 份</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>
        <div className="history-body">
          <aside className="history-list">
            {[1, 2, 3, 4].map((sid) => (
              <div key={sid} className="history-stage">
                <div className="history-stage-head">
                  Stage {sid} · {STAGE_NAMES[sid]}{' '}
                  <span className="history-count">{byStage[sid].length}</span>
                </div>
                {byStage[sid].length === 0 ? (
                  <div className="history-empty">(无记录)</div>
                ) : (
                  byStage[sid].map((r) => (
                    <button
                      key={r.id}
                      className={`history-item ${
                        selected?.id === r.id ? 'active' : ''
                      }`}
                      onClick={() => openRecord(r)}
                      title={r.path}
                    >
                      <span className="history-time">
                        {new Date(r.created_at).toLocaleString()}
                      </span>
                      <span className="history-kind">{r.kind}</span>
                    </button>
                  ))
                )}
              </div>
            ))}
          </aside>
          <section className="history-preview">
            {selected ? (
              loading ? (
                <div className="drawer-empty">读取中…</div>
              ) : error ? (
                <div className="drawer-error">⚠ {error}</div>
              ) : (
                <>
                  <div className="drawer-meta">
                    path: <code>{selected.path}</code>
                  </div>
                  <pre className="drawer-artifact">{preview}</pre>
                </>
              )
            ) : (
              <div className="drawer-empty">从左侧选一条记录查看</div>
            )}
          </section>
        </div>
      </div>
    </div>
  )
}
