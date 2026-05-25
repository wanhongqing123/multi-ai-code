import { useCallback, useEffect, useState } from 'react'
import type { Skill, SkillStep } from './skillTypes'

interface Props {
  /** Allow inline test-run only when the main session is ready. */
  sessionRunning: boolean
  /** Hand off a chosen skill to the parent's run pipeline (variables dialog or direct exec). */
  onTestRun: (skill: Skill) => void
  /** Whenever skills change, bump this so the SkillBar in the parent can refresh. */
  onChanged: () => void
}

interface EditForm {
  name: string
  trigger: string
  description: string
  steps: SkillStep[]
}

function emptyForm(): EditForm {
  return { name: '', trigger: '', description: '', steps: [] }
}

function rawToSkill(raw: {
  id: number
  name: string
  description: string | null
  trigger: string | null
  steps: unknown[]
  source: string | null
  candidateId: number | null
  createdAt: number
  updatedAt: number
  lastUsedAt: number | null
}): Skill {
  return {
    id: raw.id,
    name: raw.name,
    description: raw.description,
    trigger: raw.trigger,
    steps: (raw.steps as SkillStep[]) ?? [],
    source: (raw.source as Skill['source']) ?? null,
    candidateId: raw.candidateId,
    createdAt: raw.createdAt,
    updatedAt: raw.updatedAt,
    lastUsedAt: raw.lastUsedAt
  }
}

export default function SkillLibraryPanel(props: Props): JSX.Element {
  const { sessionRunning, onTestRun, onChanged } = props
  const [skills, setSkills] = useState<Skill[]>([])
  const [editingId, setEditingId] = useState<number | 'new' | null>(null)
  const [form, setForm] = useState<EditForm>(emptyForm())

  const refresh = useCallback(async () => {
    const list = await window.api.habit.skills.list()
    setSkills(list.map(rawToSkill))
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh])

  function startNew() {
    setEditingId('new')
    setForm(emptyForm())
  }

  function startEdit(s: Skill) {
    setEditingId(s.id)
    setForm({
      name: s.name,
      trigger: s.trigger ?? '',
      description: s.description ?? '',
      steps: s.steps.slice()
    })
  }

  function updateStep(idx: number, patch: Partial<SkillStep>) {
    setForm((f) => {
      const next = f.steps.slice()
      next[idx] = { ...next[idx], ...patch } as SkillStep
      return { ...f, steps: next }
    })
  }

  function addStep(type: SkillStep['type']) {
    setForm((f) => ({
      ...f,
      steps: [
        ...f.steps,
        type === 'prompt' ? { type: 'prompt', text: '' } : { type: 'wait-response' }
      ]
    }))
  }

  function removeStep(idx: number) {
    setForm((f) => ({ ...f, steps: f.steps.filter((_, i) => i !== idx) }))
  }

  function moveStep(idx: number, dir: -1 | 1) {
    setForm((f) => {
      const j = idx + dir
      if (j < 0 || j >= f.steps.length) return f
      const next = f.steps.slice()
      ;[next[idx], next[j]] = [next[j], next[idx]]
      return { ...f, steps: next }
    })
  }

  async function commit() {
    const cleanSteps = form.steps.filter((s) =>
      s.type === 'prompt' ? s.text.trim().length > 0 : true
    )
    if (!form.name.trim() || cleanSteps.length === 0) return
    if (editingId === 'new') {
      await window.api.habit.skills.create({
        name: form.name.trim(),
        description: form.description.trim() || null,
        trigger: form.trigger.trim() || null,
        steps: cleanSteps,
        source: 'manual'
      })
    } else if (typeof editingId === 'number') {
      await window.api.habit.skills.update(editingId, {
        name: form.name.trim(),
        description: form.description.trim() || null,
        trigger: form.trigger.trim() || null,
        steps: cleanSteps
      })
    }
    setEditingId(null)
    setForm(emptyForm())
    await refresh()
    onChanged()
  }

  async function remove(s: Skill) {
    if (!confirm(`确定要删除 skill "${s.name}" 吗？`)) return
    await window.api.habit.skills.delete(s.id)
    await refresh()
    onChanged()
  }

  return (
    <div className="habit-skill-library">
      <header className="habit-skill-library-head">
        <strong>已采纳 skills · {skills.length}</strong>
        <span className="habit-settings-actions">
          <button type="button" className="drawer-btn primary" onClick={startNew}>
            ＋ 新建
          </button>
          <button type="button" className="drawer-btn" onClick={() => void refresh()}>
            刷新
          </button>
        </span>
      </header>

      {editingId !== null && (
        <div className="habit-skill-editor">
          <label>
            Skill 名
            <input
              className="plan-name-input"
              value={form.name}
              onChange={(e) => setForm((f) => ({ ...f, name: e.target.value }))}
            />
          </label>
          <label>
            触发关键词 (可选)
            <input
              className="plan-name-input"
              placeholder="如：审查改动"
              value={form.trigger}
              onChange={(e) => setForm((f) => ({ ...f, trigger: e.target.value }))}
            />
          </label>
          <label>
            说明 (可选)
            <input
              className="plan-name-input"
              value={form.description}
              onChange={(e) => setForm((f) => ({ ...f, description: e.target.value }))}
            />
          </label>

          <div className="habit-step-list">
            {form.steps.map((s, i) => (
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
                    onChange={(e) => updateStep(i, { type: 'prompt', text: e.target.value })}
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
                        timeoutMs:
                          e.target.value === '' ? undefined : Number(e.target.value)
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
                    disabled={i === form.steps.length - 1}
                    onClick={() => moveStep(i, 1)}
                  >
                    ↓
                  </button>
                  <button className="drawer-btn warn" onClick={() => removeStep(i)}>
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
                setForm(emptyForm())
              }}
            >
              取消
            </button>
            <button
              className="drawer-btn primary"
              disabled={
                !form.name.trim() ||
                form.steps.filter((s) => s.type !== 'prompt' || s.text.trim()).length === 0
              }
              onClick={() => void commit()}
            >
              {editingId === 'new' ? '创建' : '保存'}
            </button>
          </div>
        </div>
      )}

      {skills.length === 0 && editingId === null ? (
        <div className="drawer-empty">
          还没有 skill。点「＋ 新建」手动创建，或在「候选」tab 采纳一个。
        </div>
      ) : (
        <ul className="habit-skill-list">
          {skills.map((s) => (
            <li key={s.id} className="habit-skill-item">
              <div className="habit-skill-row-head">
                <span className="habit-skill-name">{s.name}</span>
                {s.trigger && <span className="habit-candidate-trigger">/{s.trigger}</span>}
                <span className="habit-skill-meta">
                  {s.steps.length} 步
                  {s.lastUsedAt
                    ? ` · 上次使用 ${new Date(s.lastUsedAt).toLocaleDateString()}`
                    : ''}
                  {s.source ? ` · ${s.source}` : ''}
                </span>
              </div>
              {s.description && (
                <div className="habit-skill-desc">{s.description}</div>
              )}
              <ol className="habit-step-preview">
                {s.steps.map((step, i) => (
                  <li key={i}>
                    <span className="habit-step-tag">
                      {step.type === 'prompt' ? 'prompt' : '等响应'}
                    </span>
                    {step.type === 'prompt' ? (
                      <span>{step.text}</span>
                    ) : (
                      <span>等 AI 答完 ({step.timeoutMs ?? 'default'} ms)</span>
                    )}
                  </li>
                ))}
              </ol>
              <div className="drawer-actions">
                <button
                  className="drawer-btn primary"
                  disabled={!sessionRunning}
                  onClick={() => onTestRun(s)}
                  title={!sessionRunning ? '会话未启动' : '在当前会话试运行'}
                >
                  试运行
                </button>
                <button className="drawer-btn" onClick={() => startEdit(s)}>
                  编辑
                </button>
                <button className="drawer-btn warn" onClick={() => void remove(s)}>
                  删除
                </button>
              </div>
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}
