import { useCallback, useEffect, useState } from 'react'
import StagePanel from './components/StagePanel'
import CompletionDrawer from './components/CompletionDrawer'
import FeedbackDialog from './components/FeedbackDialog'
import HistoryDrawer from './components/HistoryDrawer'
import type { StageDoneEvent } from '../electron/preload'

const STAGES = [
  { id: 1, name: '方案设计' },
  { id: 2, name: '方案实施' },
  { id: 3, name: '方案验收' },
  { id: 4, name: '测试验证' }
]

/** Internal stageId → 1-based UI position (identity for stages 1-4). */
function displayIndexOf(stageId: number): number | undefined {
  return stageId >= 1 && stageId <= 4 ? stageId : undefined
}

const DEMO_PROJECT = 'demo'

/**
 * Figure out which stage follows `fromStage` given an optional verdict.
 *
 * Pipeline:
 *   1 方案设计 → 2 方案实施 → 3 方案验收 → 4 测试验证 → (done)
 *
 * Fail routes back to implementation:
 *   - Stage 3 verdict=fail → Stage 2
 *   - Stage 4 verdict=fail → Stage 2
 */
function nextStageFor(fromStage: number, verdict?: string): number | null {
  if (fromStage === 3 && verdict === 'fail') return 2
  if (fromStage === 4 && verdict === 'fail') return 2
  if (fromStage === 4) return null
  if (fromStage >= 1 && fromStage < 4) return fromStage + 1
  return null
}

export default function App() {
  const [version, setVersion] = useState<string>('')
  const [projectDir, setProjectDir] = useState<string>('')
  const [targetRepo, setTargetRepo] = useState<string>('')
  const [projectName, setProjectName] = useState<string>('')
  const [pendingDone, setPendingDone] = useState<StageDoneEvent | null>(null)
  const [zoomedStage, setZoomedStage] = useState<number | null>(null)
  const [startAllNonce, setStartAllNonce] = useState(0)
  const [killAllNonce, setKillAllNonce] = useState(0)
  const [feedbackFrom, setFeedbackFrom] = useState<number | null>(null)
  const [feedbackForcedTarget, setFeedbackForcedTarget] = useState<number | null>(null)
  const [showHistory, setShowHistory] = useState(false)
  const [pickerStage, setPickerStage] = useState<number | null>(null)
  const [stageStatus, setStageStatus] = useState<Record<number, string>>({})

  const handleStatusChange = useCallback((stageId: number, status: string) => {
    setStageStatus((prev) =>
      prev[stageId] === status ? prev : { ...prev, [stageId]: status }
    )
  }, [])

  const anyRunning = Object.values(stageStatus).some(
    (s) => s === 'running' || s === 'awaiting-confirm'
  )

  const handleToggleAll = useCallback(async () => {
    if (anyRunning) {
      await window.api.cc.killAll()
      setKillAllNonce((n) => n + 1)
    } else {
      setStartAllNonce((n) => n + 1)
    }
  }, [anyRunning])

  useEffect(() => {
    window.api.version().then(setVersion)
    window.api.demoProject().then((p) => {
      setProjectDir(p.dir)
      setTargetRepo(p.target_repo)
      // Project considered "opened" only when target_repo is an external path
      if (p.target_repo && p.target_repo !== p.dir) {
        const base = p.target_repo.split('/').filter(Boolean).pop() || ''
        setProjectName(base)
      }
    })
  }, [])

  const handleOpenProject = useCallback(async () => {
    const picked = await window.api.project.pickDir()
    if (picked.canceled || !picked.path) return
    const res = await window.api.project.setTargetRepo(picked.path)
    if (!res.ok) {
      alert(`打开项目失败：${res.error}`)
      return
    }
    setTargetRepo(res.target_repo || picked.path)
    setProjectName(res.name || picked.path.split('/').filter(Boolean).pop() || '')
  }, [])

  /** A project is "opened" only when target_repo is a real external dir. */
  const hasProject = targetRepo !== '' && targetRepo !== projectDir

  useEffect(() => {
    const off = window.api.stage.onDone((evt) => {
      setPendingDone(evt)
    })
    return off
  }, [])

  const sessionIdFor = useCallback(
    (stageId: number) => `${DEMO_PROJECT}:stage${stageId}`,
    []
  )

  /** Ensure the target stage's CC is running; spawn + wait for prompt priming. */
  const ensureStageRunning = useCallback(
    async (stageId: number, sid: string) => {
      const running = await window.api.cc.has(sid)
      if (running) return
      const res = await window.api.cc.spawn({
        sessionId: sid,
        projectId: DEMO_PROJECT,
        stageId,
        projectDir,
        cwd: projectDir
      })
      if (!res.ok) throw new Error(`spawn stage ${stageId} failed: ${res.error}`)
      // Wait for system prompt injection (PRIMING_DELAY_MS=1200) + buffer
      await new Promise((r) => setTimeout(r, 2500))
    },
    [projectDir]
  )

  const advance = useCallback(async () => {
    if (!pendingDone) return
    const next = nextStageFor(pendingDone.stageId, pendingDone.params.verdict)
    if (next === null) {
      setPendingDone(null)
      return
    }
    const nextSession = sessionIdFor(next)
    await ensureStageRunning(next, nextSession)
    const res = await window.api.stage.injectHandoff({
      sessionId: nextSession,
      fromStage: pendingDone.stageId,
      toStage: next,
      artifactPath: pendingDone.artifactPath,
      artifactContent: pendingDone.artifactContent,
      summary: pendingDone.params.summary,
      verdict: pendingDone.params.verdict,
      projectDir
    })
    if (!res.ok) throw new Error(`inject handoff failed: ${res.error}`)
    setPendingDone(null)
  }, [pendingDone, sessionIdFor, ensureStageRunning])

  /** User-initiated reverse feedback from stage N to an earlier stage. */
  const submitFeedback = useCallback(
    async (params: { toStage: number; note: string; alsoKillCurrent: boolean }) => {
      if (feedbackFrom === null) return
      const targetSession = sessionIdFor(params.toStage)
      await ensureStageRunning(params.toStage, targetSession)
      const res = await window.api.stage.injectFeedback({
        sessionId: targetSession,
        fromStage: feedbackFrom,
        toStage: params.toStage,
        note: params.note
      })
      if (!res.ok) throw new Error(`inject feedback failed: ${res.error}`)
      if (params.alsoKillCurrent) {
        await window.api.cc.kill(sessionIdFor(feedbackFrom))
      }
      setFeedbackFrom(null)
    },
    [feedbackFrom, sessionIdFor, ensureStageRunning]
  )

  /** Stage 3 fail-route: send only the user-approved review items back to Stage 2 (方案实施). */
  const advanceWithFeedback = useCallback(
    async (feedbackMd: string) => {
      if (!pendingDone) return
      const targetStage = 2
      const targetSession = sessionIdFor(targetStage)
      await ensureStageRunning(targetStage, targetSession)
      const res = await window.api.stage.injectFeedback({
        sessionId: targetSession,
        fromStage: pendingDone.stageId,
        toStage: targetStage,
        note: feedbackMd,
        artifactPath: pendingDone.artifactPath ?? undefined,
        artifactContent: pendingDone.artifactContent ?? undefined
      })
      if (!res.ok) throw new Error(`inject feedback failed: ${res.error}`)
      setPendingDone(null)
    },
    [pendingDone, sessionIdFor, ensureStageRunning]
  )

  const nextStage = pendingDone
    ? nextStageFor(pendingDone.stageId, pendingDone.params.verdict)
    : null

  return (
    <div className={`app ${pendingDone ? 'has-drawer' : ''}`}>
      <header className="topbar">
        <button
          className="topbar-btn topbar-btn-primary"
          onClick={handleOpenProject}
          title="选择项目代码仓库目录，所有 CLI 将把它作为工作目录"
        >
          📂 {hasProject ? '切换项目' : '打开项目'}
        </button>
        <h1>Multi-AI Code</h1>
        <span className="meta">
          v{version} ·{' '}
          {hasProject ? (
            <>
              <strong>{projectName}</strong> · <code title={targetRepo}>{targetRepo}</code>
            </>
          ) : (
            <span className="meta-warn">⚠ 未打开项目（请先点左上角「📂 打开项目」）</span>
          )}
        </span>
        <button
          className="topbar-btn"
          onClick={() => setShowHistory(true)}
          disabled={!hasProject}
          title="查看各阶段产物历史（每次完成自动归档）"
        >
          📋 历史
        </button>
        <button
          className={`topbar-btn ${anyRunning ? 'topbar-btn-danger' : ''}`}
          onClick={handleToggleAll}
          disabled={!hasProject}
          title={
            !hasProject
              ? '请先打开项目'
              : anyRunning
                ? '一键终止所有运行中的阶段'
                : '一键启动所有未运行的阶段'
          }
        >
          {anyRunning ? '■ 终止全部' : '▶ 启动全部'}
        </button>
        {zoomedStage !== null && (
          <button className="topbar-btn" onClick={() => setZoomedStage(null)}>
            ↙ 退出放大 (Stage {displayIndexOf(zoomedStage) ?? zoomedStage})
          </button>
        )}
      </header>

      <div className="main-split">
        <main className={`grid ${zoomedStage !== null ? 'grid-zoomed' : ''}`}>
          {STAGES.map((s, idx) => (
            <StagePanel
              key={s.id}
              stageId={s.id}
              stageName={s.name}
              displayIndex={idx + 1}
              sessionId={sessionIdFor(s.id)}
              projectId={DEMO_PROJECT}
              projectDir={projectDir}
              cwd={projectDir || '/tmp'}
              zoomed={zoomedStage === s.id}
              hidden={zoomedStage !== null && zoomedStage !== s.id}
              onToggleZoom={() =>
                setZoomedStage((cur) => (cur === s.id ? null : s.id))
              }
              autoStartNonce={hasProject ? startAllNonce : 0}
              killAllNonce={killAllNonce}
              onStatusChange={handleStatusChange}
              disabled={!hasProject}
              onRequestFeedback={
                s.id >= 2
                  ? () => {
                      setFeedbackForcedTarget(null)
                      setFeedbackFrom(s.id)
                    }
                  : undefined
              }
              onRequestRedesign={
                s.id >= 3
                  ? () => {
                      setFeedbackForcedTarget(1)
                      setFeedbackFrom(s.id)
                    }
                  : undefined
              }
              onPickHistory={s.id === 1 ? () => setPickerStage(1) : undefined}
            />
          ))}
        </main>

        {showHistory && (
          <HistoryDrawer
            projectId={DEMO_PROJECT}
            projectDir={projectDir}
            onClose={() => setShowHistory(false)}
            onRestore={async (record) => {
              const res = await window.api.artifact.restore({
                projectId: DEMO_PROJECT,
                projectDir,
                stageId: record.stage_id,
                snapshotPath: record.path,
                sessionId: sessionIdFor(record.stage_id)
              })
              if (!res.ok) {
                alert(`恢复失败：${res.error}`)
                return
              }
              setShowHistory(false)
            }}
          />
        )}
        {pickerStage !== null && (
          <HistoryDrawer
            projectId={DEMO_PROJECT}
            projectDir={projectDir}
            pickStage={pickerStage}
            onClose={() => setPickerStage(null)}
            onPick={async (snapshotPath) => {
              const res = await window.api.artifact.restore({
                projectId: DEMO_PROJECT,
                projectDir,
                stageId: pickerStage,
                snapshotPath,
                sessionId: sessionIdFor(pickerStage)
              })
              if (!res.ok) {
                alert(`恢复历史方案失败：${res.error}`)
                return
              }
              setPickerStage(null)
              // The main process already broadcast stage:done → CompletionDrawer opens.
            }}
            onImportFile={async () => {
              const res = await window.api.artifact.importFile({
                projectId: DEMO_PROJECT,
                projectDir,
                stageId: pickerStage,
                sessionId: sessionIdFor(pickerStage)
              })
              if (res.canceled) return
              if (!res.ok) {
                alert(`导入文件失败：${res.error}`)
                return
              }
              setPickerStage(null)
            }}
            onRefine={async (snapshotPath) => {
              const seeded = await window.api.artifact.seed({
                projectId: DEMO_PROJECT,
                projectDir,
                stageId: pickerStage,
                snapshotPath
              })
              if (!seeded.ok) {
                alert(`导入方案失败：${seeded.error}`)
                return
              }
              const sid = sessionIdFor(pickerStage)
              try {
                await ensureStageRunning(pickerStage, sid)
              } catch (err) {
                alert((err as Error).message)
                return
              }
              await window.api.cc.sendUser(
                sid,
                [
                  `已把上一版方案写回到 ${seeded.artifactAbs}（同一个默认产物路径）。`,
                  `请先完整读取这份方案，然后与用户交互，逐点补充 / 修正 / 完善它；`,
                  `完善完成后仍按系统 prompt 约定，覆盖写回同一路径并打印 <<STAGE_DONE ...>> 标记。`
                ].join('\n')
              )
              setPickerStage(null)
            }}
          />
        )}
        {feedbackFrom !== null && (
          <FeedbackDialog
            fromStage={feedbackFrom}
            targetOptions={
              feedbackForcedTarget !== null
                ? [feedbackForcedTarget]
                : Array.from({ length: feedbackFrom - 1 }, (_, i) => i + 1)
            }
            defaultTarget={
              feedbackForcedTarget ?? (feedbackFrom === 2 ? 1 : feedbackFrom - 1)
            }
            displayIndexOf={displayIndexOf}
            onSubmit={async (p) => {
              await submitFeedback(p)
              setFeedbackForcedTarget(null)
            }}
            onCancel={() => {
              setFeedbackFrom(null)
              setFeedbackForcedTarget(null)
            }}
          />
        )}
        {pendingDone && (
          <CompletionDrawer
            event={pendingDone}
            nextStageId={nextStage}
            nextSessionId={nextStage ? sessionIdFor(nextStage) : null}
            currentDisplayIndex={displayIndexOf(pendingDone.stageId)}
            nextDisplayIndex={nextStage ? displayIndexOf(nextStage) : undefined}
            onAdvance={advance}
            onAdvanceWithFeedback={advanceWithFeedback}
            onDismiss={() => setPendingDone(null)}
          />
        )}
      </div>

      <footer className="pipeline">
        <span>Pipeline:</span>
        {STAGES.map((s, i) => (
          <span key={s.id} className="pipe-node">
            {s.name}
            {i < STAGES.length - 1 && <span className="pipe-arrow">─▶</span>}
          </span>
        ))}
      </footer>
    </div>
  )
}
