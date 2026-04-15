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
  /** If set, only snapshots of this stage are shown and a "选用此方案" action appears. */
  pickStage?: number
  /** Called when the user chooses a snapshot in picker mode. */
  onPick?: (snapshotPath: string) => Promise<void> | void
  /** Called when the user picks "refine this plan" — seed + run CLI. */
  onRefine?: (snapshotPath: string) => Promise<void> | void
  /** Called when the user clicks the "import from file" shortcut (picker mode only). */
  onImportFile?: () => Promise<void> | void
  /** General-mode "restore into its own stage" action. */
  onRestore?: (record: ArtifactRecord) => Promise<void> | void
}

export default function HistoryDrawer({
  projectId,
  projectDir,
  onClose,
  pickStage,
  onPick,
  onRefine,
  onImportFile,
  onRestore
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
      if (pickStage !== undefined && r.stage_id !== pickStage) continue
      if (out[r.stage_id]) out[r.stage_id].push(r)
    }
    return out
  }, [records, pickStage])

  const stagesToShow = pickStage !== undefined ? [pickStage] : [1, 2, 3, 4]
  const [picking, setPicking] = useState(false)

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
          <h3>
            {pickStage !== undefined
              ? `📋 选用历史方案 · Stage ${pickStage} ${STAGE_NAMES[pickStage] ?? ''}`
              : `📋 产物历史 · ${records.length} 份`}
          </h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>
        <div className="history-body">
          <aside className="history-list">
            {pickStage !== undefined && onImportFile && (
              <button
                className="history-item history-import"
                onClick={() => void onImportFile()}
                title="选择一个本地 Markdown/文本文件作为本阶段产物"
              >
                <span className="history-time">📂 从文件导入…</span>
                <span className="history-kind">external</span>
              </button>
            )}
            {stagesToShow.map((sid) => (
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
                  {pickStage !== undefined && (onPick || onRefine) && (
                    <div className="drawer-actions">
                      {onPick && (
                        <button
                          className="drawer-btn primary"
                          disabled={picking}
                          onClick={async () => {
                            setPicking(true)
                            try {
                              await onPick(selected.path)
                            } finally {
                              setPicking(false)
                            }
                          }}
                        >
                          {picking ? '应用中…' : '✓ 选用此方案 → 进入下一阶段'}
                        </button>
                      )}
                      {onRefine && (
                        <button
                          className="drawer-btn"
                          disabled={picking}
                          onClick={async () => {
                            setPicking(true)
                            try {
                              await onRefine(selected.path)
                            } finally {
                              setPicking(false)
                            }
                          }}
                          title="把此方案落盘为当前产物并拉起 CLI，在其基础上继续对话完善"
                        >
                          {picking ? '处理中…' : '✎ 继续完善此方案'}
                        </button>
                      )}
                    </div>
                  )}
                  {pickStage === undefined && onRestore && (
                    <div className="drawer-actions">
                      <button
                        className="drawer-btn primary"
                        disabled={picking}
                        onClick={async () => {
                          setPicking(true)
                          try {
                            await onRestore(selected)
                          } finally {
                            setPicking(false)
                          }
                        }}
                        title={`把此记录作为 Stage ${selected.stage_id} 的当前产物，并弹出确认抽屉`}
                      >
                        {picking
                          ? '应用中…'
                          : `✓ 选用为 Stage ${selected.stage_id} ${STAGE_NAMES[selected.stage_id] ?? ''} 产物`}
                      </button>
                    </div>
                  )}
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
