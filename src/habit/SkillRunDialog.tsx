import { useEffect, useState } from 'react'
import type { Skill } from './skillTypes'

interface Props {
  skill: Skill
  variables: string[]
  onCancel: () => void
  onConfirm: (vars: Record<string, string>) => Promise<void> | void
}

/**
 * Modal that collects `{var}` values before running a parameterized Skill.
 * Renders one labeled input per placeholder, with the first one auto-focused
 * for fast keyboard-only use.
 */
export default function SkillRunDialog({
  skill,
  variables,
  onCancel,
  onConfirm
}: Props): JSX.Element {
  const [values, setValues] = useState<Record<string, string>>(() => {
    const seed: Record<string, string> = {}
    for (const v of variables) seed[v] = ''
    return seed
  })
  const [submitting, setSubmitting] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      if (e.key === 'Escape' && !submitting) onCancel()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onCancel, submitting])

  const allFilled = variables.every((v) => values[v]?.trim().length > 0)

  async function handleSubmit() {
    if (!allFilled || submitting) return
    setSubmitting(true)
    setError(null)
    try {
      await onConfirm(values)
    } catch (err) {
      setError((err as Error).message)
      setSubmitting(false)
    }
  }

  return (
    <div className="modal-backdrop" onClick={(e) => e.target === e.currentTarget && onCancel()}>
      <div className="modal skill-run-dialog" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>填写参数 · {skill.name}</h3>
          <button className="modal-close" onClick={onCancel}>
            ×
          </button>
        </div>
        <div className="skill-run-body">
          {skill.description && <p className="skill-run-desc">{skill.description}</p>}
          <ul className="skill-run-steps-preview">
            {skill.steps.map((s, i) => (
              <li key={i}>
                <span className="skill-run-step-tag">
                  {s.type === 'prompt' ? `Step ${i + 1} · prompt` : `Step ${i + 1} · 等响应`}
                </span>
                {s.type === 'prompt' && <span className="skill-run-step-text">{s.text}</span>}
              </li>
            ))}
          </ul>
          <div className="skill-run-vars">
            {variables.map((v, idx) => (
              <label key={v} className="skill-run-var-row">
                <span className="skill-run-var-name">{v}</span>
                <input
                  autoFocus={idx === 0}
                  className="skill-run-var-input"
                  value={values[v] ?? ''}
                  onChange={(e) =>
                    setValues((prev) => ({ ...prev, [v]: e.target.value }))
                  }
                  onKeyDown={(e) => {
                    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
                      e.preventDefault()
                      void handleSubmit()
                    }
                  }}
                />
              </label>
            ))}
          </div>
          {error && <div className="skill-run-error">{error}</div>}
        </div>
        <div className="modal-actions">
          <button className="drawer-btn" onClick={onCancel} disabled={submitting}>
            取消
          </button>
          <button
            className="drawer-btn primary"
            disabled={!allFilled || submitting}
            onClick={() => void handleSubmit()}
            title="Ctrl/Cmd+Enter"
          >
            {submitting ? '执行中…' : '执行'}
          </button>
        </div>
      </div>
    </div>
  )
}
