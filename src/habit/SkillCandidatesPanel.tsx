import { useEffect, useState } from 'react'
import {
  HABIT_KIND_LABELS,
  type HabitEventKind,
  type SkillCandidateRow
} from './habitTypes'
import type { SkillStep } from './skillTypes'

interface Props {
  candidates: SkillCandidateRow[]
  onRefresh: () => Promise<void>
  /**
   * Persist this candidate as a Skill in the platform library. The panel
   * provides title/trigger/steps; the parent handles the IPC + downstream
   * status update (so SkillBar can refresh).
   */
  onAccept: (
    candidate: SkillCandidateRow,
    payload: {
      name: string
      description?: string | null
      trigger?: string | null
      steps: SkillStep[]
    }
  ) => Promise<void>
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

interface CandidateMeta {
  steps?: SkillStep[]
  trigger?: string
  variables?: string[]
  category?: string
  rationale?: string
  source?: string
}

function parseMeta(raw: string | null): CandidateMeta {
  if (!raw) return {}
  try {
    const parsed = JSON.parse(raw) as unknown
    if (!parsed || typeof parsed !== 'object') return {}
    return parsed as CandidateMeta
  } catch {
    return {}
  }
}

/**
 * Derives a clean step list from candidate meta + the legacy body field.
 * New candidates have meta.steps; old ones get wrapped from generated_body.
 */
function deriveSteps(c: SkillCandidateRow): SkillStep[] {
  const meta = parseMeta(c.generated_meta)
  if (Array.isArray(meta.steps) && meta.steps.length > 0) {
    return meta.steps.filter(
      (s): s is SkillStep =>
        !!s &&
        typeof s === 'object' &&
        ((s as SkillStep).type === 'prompt' ||
          (s as SkillStep).type === 'wait-response')
    )
  }
  const body = (c.generated_body ?? '').trim()
  if (body) return [{ type: 'prompt', text: body }]
  return []
}

interface EditForm {
  name: string
  trigger: string
  description: string
  steps: SkillStep[]
}

function blankPromptStep(): SkillStep {
  return { type: 'prompt', text: '' }
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
  const [editForm, setEditForm] = useState<EditForm | null>(null)

  useEffect(() => {
    void onRefresh()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  function startEdit(c: SkillCandidateRow) {
    const meta = parseMeta(c.generated_meta)
    setEditingId(c.id)
    setEditForm({
      name: c.generated_title ?? '',
      trigger: meta.trigger ?? '',
      description: meta.rationale ?? '',
      steps: deriveSteps(c)
    })
  }

  function updateStep(idx: number, patch: Partial<SkillStep>) {
    setEditForm((f) => {
      if (!f) return f
      const next = f.steps.slice()
      const current = next[idx]
      // Type-narrowed merge — keep step type unless explicitly switching.
      next[idx] = { ...current, ...patch } as SkillStep
      return { ...f, steps: next }
    })
  }

  function addStep(type: SkillStep['type']) {
    setEditForm((f) => {
      if (!f) return f
      const newStep: SkillStep =
        type === 'prompt' ? blankPromptStep() : { type: 'wait-response' }
      return { ...f, steps: [...f.steps, newStep] }
    })
  }

  function removeStep(idx: number) {
    setEditForm((f) => {
      if (!f) return f
      return { ...f, steps: f.steps.filter((_, i) => i !== idx) }
    })
  }

  function moveStep(idx: number, dir: -1 | 1) {
    setEditForm((f) => {
      if (!f) return f
      const j = idx + dir
      if (j < 0 || j >= f.steps.length) return f
      const next = f.steps.slice()
      ;[next[idx], next[j]] = [next[j], next[idx]]
      return { ...f, steps: next }
    })
  }

  async function commitEdit() {
    if (!editForm || editingId == null) return
    const c = candidates.find((x) => x.id === editingId)
    if (!c) return
    const cleanSteps = editForm.steps.filter((s) => {
      if (s.type === 'prompt') return s.text.trim().length > 0
      return true
    })
    if (cleanSteps.length === 0 || !editForm.name.trim()) return
    await onAccept(c, {
      name: editForm.name.trim(),
      description: editForm.description.trim() || null,
      trigger: editForm.trigger.trim() || null,
      steps: cleanSteps
    })
    setEditingId(null)
    setEditForm(null)
  }

  /** Fast-path accept: use the candidate as-is without opening the edit form. */
  async function quickAccept(c: SkillCandidateRow) {
    const steps = deriveSteps(c)
    if (steps.length === 0 || !c.generated_title) return
    const meta = parseMeta(c.generated_meta)
    await onAccept(c, {
      name: c.generated_title,
      description: meta.rationale ?? null,
      trigger: meta.trigger ?? null,
      steps
    })
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
          <button type="button" className="drawer-btn" onClick={() => void onRefresh()}>
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
            const meta = parseMeta(c.generated_meta)
            const steps = deriveSteps(c)
            const editing = editingId === c.id && editForm

            return (
              <li key={c.id} className="habit-candidate-item">
                <div className="habit-candidate-meta">
                  <span className="habit-candidate-kind">
                    {HABIT_KIND_LABELS[c.cluster_kind as HabitEventKind] ?? c.cluster_kind}
                  </span>
                  <span>簇大小: {c.cluster_size}</span>
                  <span>{fmtTs(c.created_at)}</span>
                  {meta.trigger && (
                    <span className="habit-candidate-trigger">/{meta.trigger}</span>
                  )}
                  {c.status === 'error' && (
                    <span className="habit-candidate-error">
                      生成失败: {c.error_message ?? '未知错误'}
                    </span>
                  )}
                </div>

                {editing ? (
                  <div className="habit-candidate-editor">
                    <label>
                      Skill 名
                      <input
                        className="plan-name-input"
                        value={editForm!.name}
                        onChange={(e) =>
                          setEditForm((f) => (f ? { ...f, name: e.target.value } : f))
                        }
                      />
                    </label>
                    <label>
                      触发关键词 (可选)
                      <input
                        className="plan-name-input"
                        placeholder="例如：审查改动"
                        value={editForm!.trigger}
                        onChange={(e) =>
                          setEditForm((f) =>
                            f ? { ...f, trigger: e.target.value } : f
                          )
                        }
                      />
                    </label>
                    <label>
                      说明 (可选)
                      <input
                        className="plan-name-input"
                        value={editForm!.description}
                        onChange={(e) =>
                          setEditForm((f) =>
                            f ? { ...f, description: e.target.value } : f
                          )
                        }
                      />
                    </label>

                    <div className="habit-step-list">
                      {editForm!.steps.map((s, i) => (
                        <div key={i} className="habit-step-row">
                          <span className="habit-step-tag">
                            {s.type === 'prompt' ? `Step ${i + 1} · prompt` : `Step ${i + 1} · 等响应`}
                          </span>
                          {s.type === 'prompt' ? (
                            <textarea
                              className="habit-step-text"
                              rows={2}
                              placeholder="prompt 文本，使用 {变量名} 占位"
                              value={s.text}
                              onChange={(e) =>
                                updateStep(i, { type: 'prompt', text: e.target.value })
                              }
                            />
                          ) : (
                            <input
                              className="habit-step-text"
                              type="number"
                              placeholder="超时 ms（可选）"
                              value={s.timeoutMs ?? ''}
                              onChange={(e) =>
                                updateStep(i, {
                                  type: 'wait-response',
                                  timeoutMs: e.target.value === '' ? undefined : Number(e.target.value)
                                })
                              }
                            />
                          )}
                          <div className="habit-step-actions">
                            <button
                              className="drawer-btn"
                              disabled={i === 0}
                              onClick={() => moveStep(i, -1)}
                            >
                              ↑
                            </button>
                            <button
                              className="drawer-btn"
                              disabled={i === editForm!.steps.length - 1}
                              onClick={() => moveStep(i, 1)}
                            >
                              ↓
                            </button>
                            <button
                              className="drawer-btn warn"
                              onClick={() => removeStep(i)}
                            >
                              ×
                            </button>
                          </div>
                        </div>
                      ))}
                    </div>

                    <div className="habit-add-step-row">
                      <button className="drawer-btn" onClick={() => addStep('prompt')}>
                        ＋ 加 Prompt 步骤
                      </button>
                      <button className="drawer-btn" onClick={() => addStep('wait-response')}>
                        ＋ 加 等待响应
                      </button>
                    </div>

                    <div className="drawer-actions">
                      <button
                        className="drawer-btn"
                        onClick={() => {
                          setEditingId(null)
                          setEditForm(null)
                        }}
                      >
                        取消
                      </button>
                      <button
                        className="drawer-btn primary"
                        disabled={
                          !editForm!.name.trim() ||
                          editForm!.steps.filter((s) => s.type !== 'prompt' || s.text.trim()).length === 0
                        }
                        onClick={() => void commitEdit()}
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
                    {steps.length > 0 && (
                      <ol className="habit-step-preview">
                        {steps.map((s, i) => (
                          <li key={i}>
                            <span className="habit-step-tag">
                              {s.type === 'prompt' ? 'prompt' : '等响应'}
                            </span>
                            {s.type === 'prompt' ? (
                              <span>{s.text}</span>
                            ) : (
                              <span>等 AI 答完 ({s.timeoutMs ?? 'default'} ms)</span>
                            )}
                          </li>
                        ))}
                      </ol>
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
                        disabled={steps.length === 0 || !c.generated_title}
                        onClick={() => void quickAccept(c)}
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
