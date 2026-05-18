import { useEffect, useState } from 'react'
import { HABIT_KIND_LABELS, type HabitEventKind, type SkillCandidateRow } from './habitTypes'

interface Props {
  candidates: SkillCandidateRow[]
  onRefresh: () => Promise<void>
  onAccept: (c: SkillCandidateRow, finalTitle: string, finalBody: string) => Promise<void>
  onDiscard: (id: number) => Promise<void>
  onSnooze: (id: number) => Promise<void>
  onRunAnalysisNow: () => Promise<void>
  analysisRunning: boolean
}

function fmtTs(ts: number): string {
  return new Date(ts).toLocaleString()
}

function parseSamples(raw: string): string[] {
  try {
    const parsed = JSON.parse(raw) as unknown
    if (Array.isArray(parsed)) return parsed.filter((x): x is string => typeof x === 'string')
  } catch {
    /* ignore */
  }
  return []
}

export default function SkillCandidatesPanel(props: Props): JSX.Element {
  const {
    candidates,
    onRefresh,
    onAccept,
    onDiscard,
    onSnooze,
    onRunAnalysisNow,
    analysisRunning
  } = props
  const [editingId, setEditingId] = useState<number | null>(null)
  const [editForm, setEditForm] = useState({ title: '', body: '' })

  useEffect(() => {
    void onRefresh()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  function startEdit(c: SkillCandidateRow) {
    setEditingId(c.id)
    setEditForm({
      title: c.generated_title ?? '',
      body: c.generated_body ?? ''
    })
  }

  async function submitEdit() {
    const c = candidates.find((x) => x.id === editingId)
    if (!c) return
    await onAccept(c, editForm.title.trim(), editForm.body)
    setEditingId(null)
  }

  return (
    <div className="habit-candidates-panel">
      <header className="habit-candidates-head">
        <strong>候选 skill · {candidates.length}</strong>
        <span className="habit-settings-actions">
          <button
            type="button"
            className="drawer-btn"
            disabled={analysisRunning}
            onClick={() => void onRunAnalysisNow()}
            title="立即对当前已采集数据跑一次聚合 + 生成"
          >
            {analysisRunning ? '分析中…' : '立即分析'}
          </button>
          <button
            type="button"
            className="drawer-btn"
            onClick={() => void onRefresh()}
          >
            刷新
          </button>
        </span>
      </header>

      {candidates.length === 0 ? (
        <div className="drawer-empty">
          还没有可审阅的候选 skill。
          <br />
          系统每 24 小时自动跑一次聚合分析，你也可以点上面「立即分析」马上跑一次。
        </div>
      ) : (
        <ul className="habit-candidate-list">
          {candidates.map((c) => {
            const samples = parseSamples(c.representative_samples)
            const editing = editingId === c.id
            return (
              <li key={c.id} className="habit-candidate-item">
                <div className="habit-candidate-meta">
                  <span className="habit-candidate-kind">
                    {HABIT_KIND_LABELS[c.cluster_kind as HabitEventKind] ?? c.cluster_kind}
                  </span>
                  <span>簇大小: {c.cluster_size}</span>
                  <span>{fmtTs(c.created_at)}</span>
                  {c.status === 'error' && (
                    <span className="habit-candidate-error">
                      生成失败: {c.error_message ?? '未知错误'}
                    </span>
                  )}
                </div>

                {editing ? (
                  <div className="habit-candidate-editor">
                    <label>
                      标题
                      <input
                        className="plan-name-input"
                        value={editForm.title}
                        onChange={(e) =>
                          setEditForm((f) => ({ ...f, title: e.target.value }))
                        }
                      />
                    </label>
                    <label>
                      正文
                      <textarea
                        className="plan-name-input"
                        rows={6}
                        value={editForm.body}
                        onChange={(e) =>
                          setEditForm((f) => ({ ...f, body: e.target.value }))
                        }
                      />
                    </label>
                    <div className="drawer-actions">
                      <button className="drawer-btn" onClick={() => setEditingId(null)}>
                        取消
                      </button>
                      <button
                        className="drawer-btn primary"
                        disabled={!editForm.title.trim() || !editForm.body.trim()}
                        onClick={() => void submitEdit()}
                      >
                        保存并采纳
                      </button>
                    </div>
                  </div>
                ) : (
                  <>
                    <div className="habit-candidate-title">
                      {c.generated_title || <em>（待生成）</em>}
                    </div>
                    {c.generated_body && (
                      <pre className="habit-candidate-body">{c.generated_body}</pre>
                    )}

                    <details className="habit-candidate-evidence">
                      <summary>查看证据样本 ({samples.length})</summary>
                      <ul>
                        {samples.map((s, i) => (
                          <li key={i}>{s}</li>
                        ))}
                      </ul>
                    </details>

                    <div className="drawer-actions">
                      <button
                        className="drawer-btn primary"
                        disabled={!c.generated_title || !c.generated_body}
                        onClick={() =>
                          void onAccept(
                            c,
                            c.generated_title ?? '',
                            c.generated_body ?? ''
                          )
                        }
                      >
                        采纳
                      </button>
                      <button
                        className="drawer-btn"
                        disabled={!c.generated_title}
                        onClick={() => startEdit(c)}
                      >
                        编辑后采纳
                      </button>
                      <button
                        className="drawer-btn"
                        onClick={() => void onSnooze(c.id)}
                      >
                        暂不处理
                      </button>
                      <button
                        className="drawer-btn warn"
                        onClick={() => void onDiscard(c.id)}
                      >
                        丢弃
                      </button>
                    </div>
                  </>
                )}
              </li>
            )
          })}
        </ul>
      )}
    </div>
  )
}
