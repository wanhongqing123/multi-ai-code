import { useEffect, useMemo, useState } from 'react'
import type { ArtifactRecord } from '../../electron/preload'
import DiffView from './DiffView'

interface Version {
  label: string
  content: string
}

/** Parse an aggregate file (CURRENT + HISTORY markers) into versions, newest first. */
function parseVersions(content: string): Version[] {
  const versions: Version[] = []
  const curStart = content.indexOf('<!-- CURRENT-START -->')
  const curEnd = content.indexOf('<!-- CURRENT-END -->')
  if (curStart !== -1 && curEnd > curStart) {
    const block = content.slice(curStart + 22, curEnd).trim()
    const nl = block.indexOf('\n')
    const meta = nl >= 0 ? block.slice(0, nl).replace(/^##\s*/, '') : '当前版本'
    const body = nl >= 0 ? block.slice(nl + 1).trim() : block
    versions.push({ label: `当前版本 · ${meta}`, content: body })
  }
  const hStart = content.indexOf('<!-- HISTORY-START -->')
  const hEnd = content.indexOf('<!-- HISTORY-END -->')
  if (hStart !== -1 && hEnd > hStart) {
    const historyBody = content.slice(hStart + 22, hEnd).trim()
    // Split on "### " which is the demoted section marker
    const chunks = historyBody.split(/\n(?=### )/g)
    for (const c of chunks) {
      const nl = c.indexOf('\n')
      const meta = nl >= 0 ? c.slice(0, nl).replace(/^###\s*/, '') : '(历史)'
      const body = nl >= 0 ? c.slice(nl + 1).trim() : c
      if (body) versions.push({ label: meta, content: body })
    }
  }
  if (versions.length === 0) {
    versions.push({ label: '全文', content })
  }
  return versions
}

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
  /** Picker mode: merge selected plans via AI (stage 1 CLI). */
  onMergeViaAI?: (mergedContent: string) => Promise<void> | void
}

export default function HistoryDrawer({
  projectId,
  projectDir,
  onClose,
  pickStage,
  onPick,
  onRefine,
  onImportFile,
  onRestore,
  onMergeViaAI
}: HistoryDrawerProps) {
  const [records, setRecords] = useState<ArtifactRecord[]>([])
  const [selected, setSelected] = useState<ArtifactRecord | null>(null)
  const [preview, setPreview] = useState<string>('')
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    window.api.artifact.list(projectId).then(setRecords)
  }, [projectId])

  const [previews, setPreviews] = useState<Record<number, string>>({})
  useEffect(() => {
    if (records.length === 0) return
    let cancelled = false
    void (async () => {
      const out: Record<number, string> = {}
      await Promise.all(
        records.map(async (r) => {
          const res = await window.api.artifact.read(projectDir, r.path)
          if (!res.ok || !res.content) return
          // Prefer first heading line starting with #; fallback to first non-blank line
          const heading = res.content.match(/^#+\s+(.+)$/m)?.[1]
          const firstLine = res.content
            .split(/\r?\n/)
            .find((ln) => ln.trim() && !ln.trim().startsWith('#'))
          out[r.id] = (heading || firstLine || '').slice(0, 80)
        })
      )
      if (!cancelled) setPreviews(out)
    })()
    return () => {
      cancelled = true
    }
  }, [records, projectDir])

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
  const [checked, setChecked] = useState<Set<number>>(new Set())
  const [exporting, setExporting] = useState(false)
  const [diffMode, setDiffMode] = useState(false)
  const [leftIdx, setLeftIdx] = useState(1)
  const [rightIdx, setRightIdx] = useState(0)

  const versions = useMemo(() => (preview ? parseVersions(preview) : []), [preview])

  function toggleCheck(id: number, e: React.MouseEvent) {
    e.stopPropagation()
    setChecked((prev) => {
      const next = new Set(prev)
      if (next.has(id)) next.delete(id)
      else next.add(id)
      return next
    })
  }

  async function handleExport() {
    if (checked.size === 0) return
    setExporting(true)
    try {
      const items = records.filter((r) => checked.has(r.id))
      const parts: string[] = []
      for (const r of items) {
        const res = await window.api.artifact.read(projectDir, r.path)
        const stageName = STAGE_NAMES[r.stage_id] ?? `Stage ${r.stage_id}`
        const time = new Date(r.created_at).toLocaleString()
        parts.push(
          `# ${stageName} — ${time}\n\n` +
            `> path: \`${r.path}\`  \n> kind: ${r.kind}\n\n` +
            (res.ok ? res.content ?? '' : `⚠ 读取失败: ${res.error}`) +
            '\n'
        )
      }
      const merged = parts.join('\n---\n\n')
      const defaultName = items.length === 1
        ? items[0].path.split(/[/\\]/).pop()?.replace(/\.md$/, '') + '_导出.md'
        : `方案合集_${items.length}份.md`
      const saveRes = await window.api.saveFileAs(defaultName, merged)
      if (saveRes.ok) setChecked(new Set())
    } finally {
      setExporting(false)
    }
  }

  async function handleMergeViaAI() {
    if (checked.size === 0 || !onMergeViaAI) return
    setExporting(true)
    try {
      const items = records.filter((r) => checked.has(r.id))
      const parts: string[] = []
      for (const r of items) {
        const res = await window.api.artifact.read(projectDir, r.path)
        const stageName = STAGE_NAMES[r.stage_id] ?? `Stage ${r.stage_id}`
        const time = new Date(r.created_at).toLocaleString()
        parts.push(
          `## 方案来源：${stageName} — ${time}\n` +
            `> path: \`${r.path}\`\n\n` +
            (res.ok ? res.content ?? '' : `⚠ 读取失败: ${res.error}`) +
            '\n'
        )
      }
      const merged = `# 以下是 ${items.length} 份待合并优化的历史方案\n\n` +
        parts.join('\n---\n\n')
      await onMergeViaAI(merged)
      setChecked(new Set())
    } finally {
      setExporting(false)
    }
  }

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
          {checked.size > 0 && (
            <span style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
              {onMergeViaAI && checked.size >= 2 && (
                <button
                  className="drawer-btn warn"
                  disabled={exporting}
                  onClick={handleMergeViaAI}
                  title="把勾选的方案内容喂给 Stage 1 CLI，由 AI 合并优化为一份统一设计"
                >
                  {exporting ? '处理中…' : `🔀 AI 合并优化 (${checked.size})`}
                </button>
              )}
              <button
                className="drawer-btn primary"
                disabled={exporting}
                onClick={handleExport}
                style={{ marginRight: 8 }}
              >
                {exporting ? '导出中…' : `📤 导出选中 (${checked.size})`}
              </button>
            </span>
          )}
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
                    <div key={r.id} className="history-item-row">
                      <input
                        type="checkbox"
                        className="history-check"
                        checked={checked.has(r.id)}
                        onClick={(e) => toggleCheck(r.id, e)}
                        onChange={() => {}}
                        title="勾选后可批量导出"
                      />
                      <button
                        className={`history-item ${
                          selected?.id === r.id ? 'active' : ''
                        }`}
                        onClick={() => openRecord(r)}
                        title={`${r.path}\n\n${previews[r.id] ?? ''}`}
                      >
                        <span className="history-time">
                          {new Date(r.created_at).toLocaleString()}
                        </span>
                        {previews[r.id] && (
                          <span className="history-preview-line">{previews[r.id]}</span>
                        )}
                        <span className="history-kind">{r.kind}</span>
                      </button>
                    </div>
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
                    {versions.length > 1 && (
                      <span style={{ marginLeft: 16 }}>
                        <label style={{ marginRight: 6 }}>
                          <input
                            type="checkbox"
                            checked={diffMode}
                            onChange={(e) => setDiffMode(e.target.checked)}
                          />{' '}
                          Diff 模式
                        </label>
                        {diffMode && (
                          <>
                            <select
                              value={leftIdx}
                              onChange={(e) => setLeftIdx(Number(e.target.value))}
                              className="version-select"
                            >
                              {versions.map((v, i) => (
                                <option key={i} value={i}>
                                  旧: {v.label.slice(0, 40)}
                                </option>
                              ))}
                            </select>
                            <span style={{ margin: '0 4px' }}>→</span>
                            <select
                              value={rightIdx}
                              onChange={(e) => setRightIdx(Number(e.target.value))}
                              className="version-select"
                            >
                              {versions.map((v, i) => (
                                <option key={i} value={i}>
                                  新: {v.label.slice(0, 40)}
                                </option>
                              ))}
                            </select>
                          </>
                        )}
                      </span>
                    )}
                  </div>
                  {diffMode && versions.length > 1 ? (
                    <DiffView
                      oldText={versions[leftIdx]?.content ?? ''}
                      newText={versions[rightIdx]?.content ?? ''}
                      oldLabel={versions[leftIdx]?.label ?? ''}
                      newLabel={versions[rightIdx]?.label ?? ''}
                    />
                  ) : (
                    <pre className="drawer-artifact">{preview}</pre>
                  )}
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
