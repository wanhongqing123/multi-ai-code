import { useState } from 'react'

const STAGE_NAMES: Record<number, string> = {
  1: '方案设计',
  2: '方案实施',
  3: '方案验收',
  4: '测试验证'
}

export interface FeedbackDialogProps {
  fromStage: number
  /** Allowed target stages for the reverse feedback (typically stages < fromStage). */
  targetOptions: number[]
  /** Default target stage id. */
  defaultTarget: number
  /** Map internal stageId → UI display index. */
  displayIndexOf?: (stageId: number) => number | undefined
  onSubmit: (params: {
    toStage: number
    note: string
    alsoKillCurrent: boolean
  }) => void | Promise<void>
  onCancel: () => void
}

export default function FeedbackDialog({
  fromStage,
  targetOptions,
  defaultTarget,
  displayIndexOf,
  onSubmit,
  onCancel
}: FeedbackDialogProps) {
  const label = (id: number) => displayIndexOf?.(id) ?? id
  const [toStage, setToStage] = useState(defaultTarget)
  const [note, setNote] = useState('')
  const [alsoKill, setAlsoKill] = useState(true)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  async function handleSubmit() {
    if (!note.trim()) {
      setError('请描述问题')
      return
    }
    setBusy(true)
    setError(null)
    try {
      await onSubmit({ toStage, note: note.trim(), alsoKillCurrent: alsoKill })
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="modal-backdrop" onClick={onCancel}>
      <div className="modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>回退反馈 · 从 Stage {label(fromStage)} 回到上游</h3>
          <button className="modal-close" onClick={onCancel}>
            ×
          </button>
        </div>

        <div className="modal-body">
          <label className="modal-field">
            <span>回退到</span>
            <select
              value={toStage}
              onChange={(e) => setToStage(Number(e.target.value))}
              disabled={busy}
            >
              {targetOptions.map((id) => (
                <option key={id} value={id}>
                  Stage {label(id)} · {STAGE_NAMES[id]}
                </option>
              ))}
            </select>
          </label>

          <label className="modal-field">
            <span>问题描述（会作为 prompt 注入到目标阶段）</span>
            <textarea
              value={note}
              onChange={(e) => setNote(e.target.value)}
              placeholder="例如：方案里遗漏了多线程下的锁策略，实施时发现 race condition，请补充…"
              rows={8}
              disabled={busy}
            />
          </label>

          <label className="modal-checkbox">
            <input
              type="checkbox"
              checked={alsoKill}
              onChange={(e) => setAlsoKill(e.target.checked)}
              disabled={busy}
            />
            <span>同时停止 Stage {label(fromStage)} 的进程</span>
          </label>
        </div>

        {error && <div className="modal-error">⚠ {error}</div>}

        <div className="modal-actions">
          <button className="drawer-btn secondary" onClick={onCancel} disabled={busy}>
            取消
          </button>
          <button className="drawer-btn warn" onClick={handleSubmit} disabled={busy}>
            {busy ? '发送中…' : `发送到 Stage ${toStage}`}
          </button>
        </div>
      </div>
    </div>
  )
}
