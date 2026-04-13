import { useState } from 'react'
import type { StageDoneEvent } from '../../electron/preload'
import ReviewChecklist from './ReviewChecklist'

const STAGE_NAMES: Record<number, string> = {
  1: '方案设计',
  2: '方案实施',
  3: '方案验收',
  4: '测试验证'
}

export interface CompletionDrawerProps {
  event: StageDoneEvent
  nextSessionId: string | null
  nextStageId: number | null
  /** UI-facing display number for the current stage (1-based across active stages). */
  currentDisplayIndex?: number
  /** UI-facing display number for the next stage. */
  nextDisplayIndex?: number
  /** Forward (or fail-route) advance with the default full-artifact handoff. */
  onAdvance: () => void | Promise<void>
  /** Stage 4 fail-route advance with a user-curated subset of review items. */
  onAdvanceWithFeedback?: (feedbackMd: string) => void | Promise<void>
  onDismiss: () => void
}

export default function CompletionDrawer({
  event,
  nextSessionId,
  nextStageId,
  currentDisplayIndex,
  nextDisplayIndex,
  onAdvance,
  onAdvanceWithFeedback,
  onDismiss
}: CompletionDrawerProps) {
  const curLabel = currentDisplayIndex ?? event.stageId
  const nextLabel = nextDisplayIndex ?? nextStageId
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const verdict = event.params.verdict
  const isFailVerdict = verdict === 'fail'
  const canAdvance = nextSessionId !== null && nextStageId !== null
  // Stage 3 (方案验收) fail → render the item-checklist view instead of plain artifact
  const isReviewFail =
    event.stageId === 3 && isFailVerdict && Boolean(event.artifactContent) && onAdvanceWithFeedback

  async function doAdvance() {
    setBusy(true)
    setError(null)
    try {
      await onAdvance()
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setBusy(false)
    }
  }

  async function doAdvanceWithFeedback(feedback: string) {
    if (!onAdvanceWithFeedback) return
    setBusy(true)
    setError(null)
    try {
      await onAdvanceWithFeedback(feedback)
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setBusy(false)
    }
  }

  return (
    <aside className="drawer">
      <div className="drawer-head">
        <div className="drawer-title">
          <span className="drawer-stage">Stage {curLabel}</span>
          <span className="drawer-stage-name">{STAGE_NAMES[event.stageId]}</span>
          <span className="drawer-done-badge">完成</span>
          {verdict && (
            <span className={`drawer-verdict ${verdict === 'pass' ? 'pass' : 'fail'}`}>
              verdict: {verdict}
            </span>
          )}
        </div>
        <button className="drawer-close" onClick={onDismiss} title="关闭抽屉">
          ×
        </button>
      </div>

      {event.artifactPath && (
        <div className="drawer-meta">
          artifact: <code>{event.artifactPath}</code>
        </div>
      )}
      {event.params.summary && (
        <div className="drawer-meta">
          summary: <code>{event.params.summary}</code>
        </div>
      )}

      <div className="drawer-body">
        {isReviewFail ? (
          <ReviewChecklist
            artifactContent={event.artifactContent ?? ''}
            targetStageLabel={nextLabel ?? 3}
            onAccept={(fb) => doAdvanceWithFeedback(fb)}
            onDismiss={onDismiss}
            busy={busy}
          />
        ) : event.artifactContent ? (
          <pre className="drawer-artifact">{event.artifactContent}</pre>
        ) : (
          <div className="drawer-empty">
            {event.artifactPath
              ? `(未找到产物文件: ${event.artifactPath})`
              : '(本阶段未声明产物文件)'}
          </div>
        )}
      </div>

      {error && <div className="drawer-error">⚠ {error}</div>}

      {!isReviewFail && (
        <div className="drawer-actions">
          <button className="drawer-btn secondary" onClick={onDismiss} disabled={busy}>
            稍后决定
          </button>
          {canAdvance && (
            <button
              className={`drawer-btn ${isFailVerdict ? 'warn' : 'primary'}`}
              onClick={doAdvance}
              disabled={busy}
            >
              {busy
                ? '注入中…'
                : isFailVerdict
                  ? `回退到 Stage ${nextLabel} 修复`
                  : `确认 → 进入 Stage ${nextLabel}`}
            </button>
          )}
        </div>
      )}
    </aside>
  )
}
