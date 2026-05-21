import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  ALL_KB_TIERS,
  KB_TIER_LABELS,
  type KbEntry,
  type KbSearchResult,
  type KbTier
} from './kbTypes'

interface Props {
  /** The current project's target_repo absolute path. */
  repoPath: string
  onClose: () => void
}

type Tab = 'overview' | 'topics' | 'search' | 'settings'

interface KbStats {
  total: number
  byTier: Record<KbTier, number>
  approxBytes: number
  lastSummaryAt: number
  lastCompactionAt: number
}

interface KbMeta {
  repo_path: string
  last_summary_at: number
  last_compaction_at: number
  digest: string
}

interface KbSchedulerStatus {
  signals: {
    lastSummaryAt: number
    lastAiActivityAt: number
    lastUserPromptAt: number
    pendingSignalCount: number
    mainSessionRunning: boolean
    cliConfigured: boolean
  }
  nextActionable: string | null
  willRunReason: string | null
}

function fmtTs(ts: number): string {
  if (!ts || ts <= 0) return '从未'
  return new Date(ts).toLocaleString()
}

function fmtRelative(ts: number, now: number = Date.now()): string {
  if (!ts || ts <= 0) return '从未'
  const diff = Math.max(0, now - ts)
  const m = Math.floor(diff / 60000)
  if (m < 1) return '刚刚'
  if (m < 60) return `${m} 分钟前`
  const h = Math.floor(m / 60)
  if (h < 24) return `${h} 小时前`
  const d = Math.floor(h / 24)
  return `${d} 天前`
}

export default function ProjectKbDialog({ repoPath, onClose }: Props): JSX.Element {
  const [tab, setTab] = useState<Tab>('overview')
  const [stats, setStats] = useState<KbStats | null>(null)
  const [meta, setMeta] = useState<KbMeta | null>(null)
  const [schedulerStatus, setSchedulerStatus] = useState<KbSchedulerStatus | null>(null)
  const [entries, setEntries] = useState<KbEntry[]>([])
  const [searchQuery, setSearchQuery] = useState('')
  const [searchResults, setSearchResults] = useState<KbSearchResult[]>([])
  const [busy, setBusy] = useState(false)
  const [toast, setToast] = useState<string | null>(null)
  const [editingId, setEditingId] = useState<number | null>(null)
  const [editForm, setEditForm] = useState({ topic: '', summary: '' })

  function flash(msg: string) {
    setToast(msg)
    setTimeout(() => setToast(null), 2200)
  }

  const refreshAll = useCallback(async () => {
    const [s, m, ent, st] = await Promise.all([
      window.api.kb.stats({ repoPath }),
      window.api.kb.meta({ repoPath }),
      window.api.kb.list({ repoPath }),
      window.api.kb.schedulerStatus({ repoPath })
    ])
    setStats(s)
    setMeta(m)
    setEntries(ent as KbEntry[])
    setSchedulerStatus(st)
  }, [repoPath])

  useEffect(() => {
    void refreshAll()
  }, [refreshAll])

  const grouped = useMemo(() => {
    const by: Record<KbTier, KbEntry[]> = {
      pinned: [],
      hot: [],
      warm: [],
      cold: []
    }
    for (const e of entries) by[e.tier].push(e)
    return by
  }, [entries])

  async function handleRunNow() {
    if (busy) return
    setBusy(true)
    try {
      const r = await window.api.kb.runNow({ repoPath })
      if (!r.ok) {
        flash(`总结失败：${r.error}`)
      } else if (r.outcome.ran) {
        flash(
          `已总结：新增 ${r.outcome.topicsCreated ?? 0} 条 / 更新 ${r.outcome.topicsUpdated ?? 0} 条`
        )
        await refreshAll()
      } else {
        flash(`跳过：${r.outcome.reason}${r.outcome.error ? ' · ' + r.outcome.error : ''}`)
      }
    } finally {
      setBusy(false)
    }
  }

  async function handleCompactNow() {
    if (busy) return
    setBusy(true)
    try {
      const r = await window.api.kb.compactNow({ repoPath })
      if (r.ok) {
        flash(
          `压缩完成：合并 ${r.result.merged} 组，hot→warm ${r.result.demotedFromHot}，warm→cold ${r.result.demotedFromWarm}`
        )
        await refreshAll()
      } else {
        flash(`压缩失败：${r.error}`)
      }
    } finally {
      setBusy(false)
    }
  }

  async function handleClear() {
    if (!confirm('确定要清空这个项目的所有知识库内容吗？此操作不可撤销。')) return
    setBusy(true)
    try {
      const r = await window.api.kb.clear({ repoPath })
      flash(`已清空 ${r.removed} 条`)
      await refreshAll()
    } finally {
      setBusy(false)
    }
  }

  async function handleSearch() {
    if (!searchQuery.trim()) {
      setSearchResults([])
      return
    }
    const r = (await window.api.kb.search({ repoPath, query: searchQuery, limit: 20 })) as KbSearchResult[]
    setSearchResults(r)
  }

  async function handlePin(id: number, currentTier: KbTier) {
    if (currentTier === 'pinned') {
      await window.api.kb.unpin({ id })
      flash('已取消 pin')
    } else {
      await window.api.kb.pin({ id })
      flash('已 pin')
    }
    await refreshAll()
  }

  async function handleDelete(id: number) {
    if (!confirm('确定要删除这条知识库条目吗？')) return
    await window.api.kb.delete({ id })
    flash('已删除')
    await refreshAll()
  }

  function startEdit(e: KbEntry) {
    setEditingId(e.id)
    setEditForm({ topic: e.topic, summary: e.summary })
  }

  async function commitEdit() {
    if (editingId == null) return
    await window.api.kb.update({
      id: editingId,
      topic: editForm.topic.trim(),
      summary: editForm.summary
    })
    setEditingId(null)
    flash('已保存')
    await refreshAll()
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal templates-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>🧠 项目记忆</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="kb-source-hint">
          本面板对应仓库：<code title={repoPath}>{repoPath}</code>
        </div>

        <div className="habit-tabs">
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'overview' ? 'active' : ''}`}
            onClick={() => setTab('overview')}
          >
            总览
          </button>
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'topics' ? 'active' : ''}`}
            onClick={() => setTab('topics')}
          >
            主题
          </button>
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'search' ? 'active' : ''}`}
            onClick={() => setTab('search')}
          >
            搜索
          </button>
          <button
            type="button"
            className={`habit-tab-btn ${tab === 'settings' ? 'active' : ''}`}
            onClick={() => setTab('settings')}
          >
            设置
          </button>
        </div>

        <div className="habit-tab-body">
          {tab === 'overview' && stats && meta && schedulerStatus && (
            <div className="kb-overview">
              <section className="kb-overview-block">
                <strong>项目摘要</strong>
                <pre className="kb-digest">{meta.digest || '（还没有生成摘要 — 试试下方"立即总结"）'}</pre>
              </section>

              <section className="kb-overview-block">
                <strong>各 tier 占用</strong>
                <div className="kb-tier-bars">
                  {ALL_KB_TIERS.map((t) => (
                    <div key={t} className="kb-tier-bar-row">
                      <span className="kb-tier-label">{KB_TIER_LABELS[t]}</span>
                      <span className="kb-tier-count">{stats.byTier[t]}</span>
                    </div>
                  ))}
                </div>
                <div className="kb-stats-row">
                  <span>共 {stats.total} 条</span>
                  <span>上次总结：{fmtRelative(stats.lastSummaryAt)}</span>
                  <span>上次压缩：{fmtRelative(stats.lastCompactionAt)}</span>
                </div>
              </section>

              <section className="kb-overview-block">
                <strong>下一次自动总结</strong>
                <div className="kb-sched-line">
                  {schedulerStatus.willRunReason ? (
                    <span className="kb-sched-will-run">
                      ✓ 满足条件（{schedulerStatus.willRunReason}），下一个 tick 触发
                    </span>
                  ) : (
                    <span className="kb-sched-skip">
                      暂跳过：{schedulerStatus.nextActionable}
                    </span>
                  )}
                </div>
                <div className="kb-sched-line">
                  待消化提示：{schedulerStatus.signals.pendingSignalCount} 条
                </div>
              </section>

              <div className="drawer-actions">
                <button
                  className="drawer-btn primary"
                  disabled={busy}
                  onClick={() => void handleRunNow()}
                >
                  立即总结
                </button>
                <button
                  className="drawer-btn"
                  disabled={busy}
                  onClick={() => void handleCompactNow()}
                >
                  立即压缩
                </button>
                <button
                  className="drawer-btn"
                  disabled={busy}
                  onClick={() => void refreshAll()}
                >
                  刷新
                </button>
              </div>
            </div>
          )}

          {tab === 'topics' && (
            <div className="kb-topics">
              {entries.length === 0 ? (
                <div className="drawer-empty">
                  还没有任何条目。可以在「总览」点「立即总结」试试。
                </div>
              ) : (
                ALL_KB_TIERS.map((t) =>
                  grouped[t].length === 0 ? null : (
                    <section key={t} className="kb-tier-group">
                      <h4>
                        {KB_TIER_LABELS[t]}{' '}
                        <span className="kb-tier-count">({grouped[t].length})</span>
                      </h4>
                      <ul className="kb-entry-list">
                        {grouped[t].map((e) => (
                          <li key={e.id} className="kb-entry-row">
                            {editingId === e.id ? (
                              <div className="kb-entry-editor">
                                <input
                                  className="plan-name-input"
                                  value={editForm.topic}
                                  onChange={(ev) =>
                                    setEditForm((f) => ({
                                      ...f,
                                      topic: ev.target.value
                                    }))
                                  }
                                />
                                <textarea
                                  className="plan-name-input"
                                  rows={4}
                                  value={editForm.summary}
                                  onChange={(ev) =>
                                    setEditForm((f) => ({
                                      ...f,
                                      summary: ev.target.value
                                    }))
                                  }
                                />
                                <div className="drawer-actions">
                                  <button
                                    className="drawer-btn"
                                    onClick={() => setEditingId(null)}
                                  >
                                    取消
                                  </button>
                                  <button
                                    className="drawer-btn primary"
                                    disabled={!editForm.topic.trim() || !editForm.summary.trim()}
                                    onClick={() => void commitEdit()}
                                  >
                                    保存
                                  </button>
                                </div>
                              </div>
                            ) : (
                              <>
                                <div className="kb-entry-head">
                                  <span className="kb-entry-topic">{e.topic}</span>
                                  <span className="kb-entry-meta">
                                    {fmtRelative(e.updatedAt)} · access {e.accessCount}
                                  </span>
                                </div>
                                <div className="kb-entry-summary">{e.summary}</div>
                                {(e.evidence.commits?.length ||
                                  e.evidence.files?.length) && (
                                  <div className="kb-entry-evidence">
                                    {e.evidence.commits?.slice(0, 3).map((c) => (
                                      <code key={c}>{c}</code>
                                    ))}
                                    {e.evidence.files?.slice(0, 3).map((f) => (
                                      <code key={f}>{f}</code>
                                    ))}
                                  </div>
                                )}
                                <div className="kb-entry-actions">
                                  <button
                                    type="button"
                                    className="kb-entry-btn"
                                    onClick={() => void handlePin(e.id, e.tier)}
                                  >
                                    {e.tier === 'pinned' ? '取消 pin' : '⭐ pin'}
                                  </button>
                                  <button
                                    type="button"
                                    className="kb-entry-btn"
                                    onClick={() => startEdit(e)}
                                  >
                                    ✎ 编辑
                                  </button>
                                  <button
                                    type="button"
                                    className="kb-entry-btn warn"
                                    onClick={() => void handleDelete(e.id)}
                                  >
                                    × 删除
                                  </button>
                                </div>
                              </>
                            )}
                          </li>
                        ))}
                      </ul>
                    </section>
                  )
                )
              )}
            </div>
          )}

          {tab === 'search' && (
            <div className="kb-search">
              <div className="kb-search-bar">
                <input
                  className="plan-name-input"
                  placeholder="搜索主题 / 摘要 …"
                  value={searchQuery}
                  onChange={(e) => setSearchQuery(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') void handleSearch()
                  }}
                />
                <button
                  type="button"
                  className="drawer-btn primary"
                  onClick={() => void handleSearch()}
                >
                  搜索
                </button>
              </div>
              {searchResults.length === 0 ? (
                <div className="drawer-empty">
                  {searchQuery ? '没有命中' : '输入关键词后回车'}
                </div>
              ) : (
                <ul className="kb-entry-list">
                  {searchResults.map((r) => (
                    <li key={r.entry.id} className="kb-entry-row">
                      <div className="kb-entry-head">
                        <span className="kb-entry-topic">{r.entry.topic}</span>
                        <span className="kb-entry-meta">
                          score {r.score.toFixed(2)} · {fmtRelative(r.entry.updatedAt)}
                        </span>
                      </div>
                      <div className="kb-entry-summary">{r.entry.summary}</div>
                    </li>
                  ))}
                </ul>
              )}
            </div>
          )}

          {tab === 'settings' && schedulerStatus && (
            <div className="kb-settings">
              <section className="habit-settings-section">
                <header className="habit-settings-section-head">
                  <strong>调度信号</strong>
                </header>
                <pre className="kb-debug-block">
                  {JSON.stringify(schedulerStatus.signals, null, 2)}
                </pre>
              </section>

              <section className="habit-settings-section">
                <header className="habit-settings-section-head">
                  <strong>清空</strong>
                  <span className="habit-settings-actions">
                    <button
                      type="button"
                      className="drawer-btn warn"
                      disabled={busy}
                      onClick={() => void handleClear()}
                    >
                      清空本项目所有知识库
                    </button>
                  </span>
                </header>
                <div className="habit-settings-hint">
                  本操作只清这一个仓库的 KB，不影响其他项目；不可撤销。
                </div>
              </section>
            </div>
          )}
        </div>

        {toast && <div className="habit-toast">{toast}</div>}
      </div>
    </div>
  )
}
