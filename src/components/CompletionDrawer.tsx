import { useEffect, useState } from 'react'
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
  /** Target repo path for git operations; if absent, git section is hidden. */
  targetRepo?: string
  /** Plan name for auto-branch naming. */
  planName?: string
}

export default function CompletionDrawer({
  event,
  nextSessionId,
  nextStageId,
  currentDisplayIndex,
  nextDisplayIndex,
  onAdvance,
  onAdvanceWithFeedback,
  onDismiss,
  targetRepo,
  planName
}: CompletionDrawerProps) {
  const curLabel = currentDisplayIndex ?? event.stageId
  const nextLabel = nextDisplayIndex ?? nextStageId
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [gitBranch, setGitBranch] = useState<string>('')
  const [gitFiles, setGitFiles] = useState<{ status: string; path: string }[]>([])
  const [gitMsg, setGitMsg] = useState('')
  const [gitBusy, setGitBusy] = useState(false)
  const canGit = !!targetRepo && event.stageId >= 2

  useEffect(() => {
    if (!canGit || !targetRepo) return
    void window.api.git.status(targetRepo).then((r) => {
      if (r.ok) {
        setGitBranch(r.branch ?? '')
        setGitFiles(r.files ?? [])
      }
    })
    const stageNames: Record<number, string> = { 2: '实施', 3: '验收', 4: '测试' }
    const def = `stage${event.stageId}${stageNames[event.stageId] ? '-' + stageNames[event.stageId] : ''}: ${event.params.summary ?? '(自动提交)'}${planName ? ` [${planName}]` : ''}`
    setGitMsg(def)
  }, [canGit, targetRepo, event.stageId, event.params.summary, planName])

  async function doCommit() {
    if (!targetRepo) return
    setGitBusy(true)
    const res = await window.api.git.commit(targetRepo, gitMsg)
    setGitBusy(false)
    if (!res.ok) {
      setError(`git commit 失败：${res.error}`)
      return
    }
    const s = await window.api.git.status(targetRepo)
    if (s.ok) {
      setGitBranch(s.branch ?? '')
      setGitFiles(s.files ?? [])
    }
  }

  async function doBranchCheckout() {
    if (!targetRepo || !planName) return
    const name = `multiai/${planName.replace(/[^\w-]+/g, '_')}/stage${event.stageId}`
    setGitBusy(true)
    const res = await window.api.git.checkoutBranch(targetRepo, name)
    setGitBusy(false)
    if (!res.ok) {
      setError(`git checkout 失败：${res.error}`)
      return
    }
    const s = await window.api.git.status(targetRepo)
    if (s.ok) setGitBranch(s.branch ?? '')
  }

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

      {canGit && (
        <div className="drawer-git">
          <div className="drawer-git-head">
            <span>🔧 Git</span>
            <span className="drawer-git-branch">分支: {gitBranch || '(无)'}</span>
            <span style={{ marginLeft: 'auto' }}>
              {gitFiles.length === 0 ? '工作区干净' : `${gitFiles.length} 处改动`}
            </span>
          </div>
          {gitFiles.length > 0 && (
            <div className="drawer-git-files">
              {gitFiles.slice(0, 8).map((f, i) => (
                <div key={i} className="drawer-git-file">
                  <span className="drawer-git-status">{f.status}</span> {f.path}
                </div>
              ))}
              {gitFiles.length > 8 && <div className="drawer-git-file">… +{gitFiles.length - 8} more</div>}
            </div>
          )}
          <input
            className="plan-name-input"
            value={gitMsg}
            onChange={(e) => setGitMsg(e.target.value)}
            placeholder="commit message"
          />
          <div className="drawer-git-actions">
            {planName && (
              <button className="drawer-btn" onClick={doBranchCheckout} disabled={gitBusy}>
                切到分支 multiai/{planName}/stage{event.stageId}
              </button>
            )}
            <button className="drawer-btn primary" onClick={doCommit} disabled={gitBusy || gitFiles.length === 0}>
              {gitBusy ? '处理中…' : `📝 add + commit (${gitFiles.length})`}
            </button>
          </div>
        </div>
      )}

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
