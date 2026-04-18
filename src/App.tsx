import { useCallback, useEffect, useRef, useState } from 'react'
import StagePanel from './components/StagePanel'
import CompletionDrawer from './components/CompletionDrawer'
import FeedbackDialog from './components/FeedbackDialog'
import HistoryDrawer from './components/HistoryDrawer'
import ProjectPicker, { type ProjectInfo } from './components/ProjectPicker'
import ErrorPanel, { pushLog, useLogs } from './components/ErrorPanel'
import StageSettingsDialog from './components/StageSettingsDialog'
import TemplatesDialog from './components/TemplatesDialog'
import TimelineDrawer from './components/TimelineDrawer'
import OnboardingWizard from './components/OnboardingWizard'
import DoctorDialog from './components/DoctorDialog'
import CommandPalette, { type Command } from './components/CommandPalette'
import ToastHost, { showToast } from './components/Toast'
import GlobalSearchDialog from './components/GlobalSearchDialog'
import FilePreviewDialog from './components/FilePreviewDialog'
import PlanReviewDialog, { type Annotation } from './components/PlanReviewDialog'
import type { StageDoneEvent } from '../electron/preload'

const LAST_PROJECT_KEY = 'multi-ai-code.lastProjectId'
const PLAN_NAME_KEY_PREFIX = 'multi-ai-code.planName.'

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
function nextStageFor(
  fromStage: number,
  verdict?: string,
  skips?: Record<number, boolean>
): number | null {
  const isSkipped = (s: number) => !!skips?.[s]
  let candidate: number | null
  if (fromStage === 3 && verdict === 'fail') candidate = 2
  else if (fromStage === 4 && verdict === 'fail') candidate = 2
  else if (fromStage === 4) candidate = null
  else if (fromStage >= 1 && fromStage < 4) candidate = fromStage + 1
  else candidate = null
  // Hop over skipped stages (forward only)
  while (candidate !== null && candidate > fromStage && isSkipped(candidate)) {
    if (candidate >= 4) return null
    candidate = candidate + 1
  }
  return candidate
}

export default function App() {
  const [version, setVersion] = useState<string>('')
  const [projects, setProjects] = useState<ProjectInfo[]>([])
  const [currentProjectId, setCurrentProjectId] = useState<string | null>(null)
  const [showProjectPicker, setShowProjectPicker] = useState(false)
  const [pendingDone, setPendingDone] = useState<StageDoneEvent | null>(null)
  const [zoomedStage, setZoomedStage] = useState<number | null>(null)
  const [killAllNonce, setKillAllNonce] = useState(0)
  const [feedbackFrom, setFeedbackFrom] = useState<number | null>(null)
  const [feedbackForcedTarget, setFeedbackForcedTarget] = useState<number | null>(null)
  const [showHistory, setShowHistory] = useState(false)
  const [pickerStage, setPickerStage] = useState<number | null>(null)
  const [planName, setPlanName] = useState('')
  const [showErrors, setShowErrors] = useState(false)
  const [showStageSettings, setShowStageSettings] = useState(false)
  const [showTemplates, setShowTemplates] = useState(false)
  const [showTimeline, setShowTimeline] = useState(false)
  const [showOnboarding, setShowOnboarding] = useState(false)
  const [showDoctor, setShowDoctor] = useState(false)
  const [showCmdk, setShowCmdk] = useState(false)
  const [showGlobalSearch, setShowGlobalSearch] = useState(false)
  const [stageConfigs, setStageConfigs] = useState<
    Record<string, { command?: string; args?: string[]; env?: Record<string, string>; skip?: boolean }>
  >({})
  const [planNameSuggestions, setPlanNameSuggestions] = useState<string[]>([])
  const [planStagesDone, setPlanStagesDone] = useState<Record<number, boolean>>({})
  const [msysEnabled, setMsysEnabled] = useState(false)
  // Stage-1 external-file preview state. When set, FilePreviewDialog is shown
  // and the user can confirm/cancel before the content becomes the stage artifact.
  const [previewImport, setPreviewImport] = useState<{
    path: string
    content: string
    stageId: number
  } | null>(null)
  // Stage-1 plan-review state. When set, PlanReviewDialog renders the current
  // in-progress plan md; user annotates and the annotations get sent back to
  // the Stage 1 CLI so it can revise the plan.
  const [planReview, setPlanReview] = useState<{
    path: string
    content: string
  } | null>(null)
  // Remember which (project, planName) combos have already seen the starter
  // "import from file?" toast so it's not shown repeatedly on each start.
  const shownStarterHintsRef = useRef<Set<string>>(new Set())

  // Refresh plan-name suggestions + per-stage "done" status when project/plan changes.
  useEffect(() => {
    if (!currentProjectId) {
      setPlanNameSuggestions([])
      setPlanStagesDone({})
      return
    }
    let cancelled = false
    void (async () => {
      const all = await window.api.artifact.list(currentProjectId)
      if (cancelled) return
      const names = new Set<string>()
      const stageSafe = planName.replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').replace(/\s+/g, '_').slice(0, 80)
      const done: Record<number, boolean> = { 1: false, 2: false, 3: false, 4: false }
      for (const r of all) {
        const m = r.path.match(/artifacts[/\\]history[/\\]stage(\d+)[/\\](.+?)(_\d[^.]*)?\.md$/)
        if (m) {
          const stage = Number(m[1])
          const decoded = m[2].replace(/_/g, ' ')
          names.add(decoded)
          if (stageSafe && m[2] === stageSafe) done[stage] = true
        }
      }
      setPlanNameSuggestions(Array.from(names).sort())
      setPlanStagesDone(done)
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId, pendingDone, planName])

  const stageSkips: Record<number, boolean> = {
    1: !!stageConfigs['1']?.skip,
    2: !!stageConfigs['2']?.skip,
    3: !!stageConfigs['3']?.skip,
    4: !!stageConfigs['4']?.skip
  }
  const [logs] = useLogs()
  const errorCount = logs.filter((l) => l.level === 'error' || l.level === 'warn').length
  const [stageStatus, setStageStatus] = useState<Record<number, string>>({})

  /** Ensure planName is set; returns false if user cancelled. */
  function requirePlanName(): string | null {
    if (planName.trim()) return planName.trim()
    const input = window.prompt('请输入本次方案名称（必填，用于归档时识别）：')
    if (!input?.trim()) return null
    setPlanName(input.trim())
    return input.trim()
  }

  const handleStatusChange = useCallback((stageId: number, status: string) => {
    setStageStatus((prev) =>
      prev[stageId] === status ? prev : { ...prev, [stageId]: status }
    )
  }, [])

  const reloadProjects = useCallback(async () => {
    const list = await window.api.project.list()
    setProjects(list)
    return list
  }, [])

  /** Clear UI state tied to a specific project (drawers, dialogs, plan name). */
  const clearProjectScopedState = useCallback(() => {
    setPendingDone(null)
    setFeedbackFrom(null)
    setFeedbackForcedTarget(null)
    setPickerStage(null)
    setShowHistory(false)
    setShowGlobalSearch(false)
    setZoomedStage(null)
    setPlanName('')
    setPreviewImport(null)
    shownStarterHintsRef.current.clear()
  }, [])

  /** Open file picker, read content, and queue it in the preview dialog. */
  const pickExternalFileForPreview = useCallback(
    async (stageId: number) => {
      const pick = await window.api.dialog.pickTextFile({
        title: stageId === 1 ? '选择方案设计文档' : '选择要导入的文件'
      })
      if (pick.canceled) return
      if (pick.error || !pick.path || pick.content === undefined) {
        alert(`读取文件失败：${pick.error ?? '未知错误'}`)
        return
      }
      setPreviewImport({ path: pick.path, content: pick.content, stageId })
    },
    []
  )

  // Global keyboard shortcuts
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const mod = e.ctrlKey || e.metaKey
      if (mod && !e.shiftKey && e.key.toLowerCase() === 'k') {
        e.preventDefault()
        setShowCmdk((s) => !s)
      } else if (mod && e.shiftKey && e.key.toLowerCase() === 'f') {
        e.preventDefault()
        setShowGlobalSearch(true)
      } else if (mod && !e.shiftKey && /^[1-4]$/.test(e.key)) {
        // Avoid stealing focus when typing in inputs
        const target = e.target as HTMLElement | null
        if (target && ['INPUT', 'TEXTAREA'].includes(target.tagName)) return
        e.preventDefault()
        const n = Number(e.key)
        setZoomedStage((cur) => (cur === n ? null : n))
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  useEffect(() => {
    // One-shot Doctor on startup: log any missing CLI into the error panel
    void window.api.doctor.check().then((results) => {
      for (const r of results) {
        if (!r.ok && r.required) {
          pushLog('warn', `Doctor:${r.name}`, `${r.error ?? '未就绪'} — ${r.install}`)
        }
      }
    })
    window.api.version().then(setVersion)
    void (async () => {
      const list = await reloadProjects()
      const last = localStorage.getItem(LAST_PROJECT_KEY)
      const pick = list.find((p) => p.id === last) ?? list[0]
      if (pick) {
        setCurrentProjectId(pick.id)
        const saved = localStorage.getItem(PLAN_NAME_KEY_PREFIX + pick.id)
        if (saved) setPlanName(saved)
        if (last && last !== pick.id) {
          showToast(`上次打开的项目已不存在，已切换到「${pick.name}」`, { level: 'warn' })
        }
      } else if (!localStorage.getItem('multi-ai-code.onboarding-done')) {
        setShowOnboarding(true)
      } else {
        setShowProjectPicker(true)
      }
    })()
  }, [reloadProjects])

  const currentProject = projects.find((p) => p.id === currentProjectId) ?? null
  const projectDir = currentProject?.dir ?? ''
  const targetRepo = currentProject?.target_repo ?? ''
  const projectName = currentProject?.name ?? ''
  const hasProject = currentProject !== null

  useEffect(() => {
    if (!currentProjectId) {
      setStageConfigs({})
      setMsysEnabled(false)
      return
    }
    localStorage.setItem(LAST_PROJECT_KEY, currentProjectId)
    void window.api.project.touch(currentProjectId)
    let cancelled = false
    void window.api.project.getStageConfigs(currentProjectId).then((cfg) => {
      if (cancelled) return
      setStageConfigs(cfg)
    })
    void window.api.project.getMsysEnabled(currentProjectId).then((enabled) => {
      if (cancelled) return
      setMsysEnabled(enabled)
    })
    return () => {
      cancelled = true
    }
  }, [currentProjectId])

  // Persist planName per project so it survives restart / project switch.
  useEffect(() => {
    if (!currentProjectId) return
    const key = PLAN_NAME_KEY_PREFIX + currentProjectId
    if (planName.trim()) {
      localStorage.setItem(key, planName.trim())
    } else {
      localStorage.removeItem(key)
    }
  }, [currentProjectId, planName])

  // When Stage 1 starts running with a plan that has no historical design,
  // offer a one-shot import shortcut so the user isn't forced into a fresh
  // design. Shown once per (project, planName) combo per session.
  useEffect(() => {
    if (!currentProjectId || !planName.trim()) return
    if (stageStatus[1] !== 'running') return
    if (planStagesDone[1]) return
    const key = `${currentProjectId}:${planName.trim()}`
    if (shownStarterHintsRef.current.has(key)) return
    shownStarterHintsRef.current.add(key)
    showToast(
      `本次方案「${planName.trim()}」尚无设计稿，可直接对话设计，或从文件导入已有方案`,
      {
        level: 'info',
        duration: 0,
        action: {
          label: '📁 从文件导入',
          onClick: () => {
            void pickExternalFileForPreview(1)
          }
        }
      }
    )
  }, [currentProjectId, planName, stageStatus, planStagesDone, pickExternalFileForPreview])

  useEffect(() => {
    const off = window.api.stage.onDone((evt) => {
      setPendingDone(evt)
      const stageName = STAGES.find((s) => s.id === evt.stageId)?.name ?? `Stage ${evt.stageId}`
      showToast(`✓ ${stageName} 完成${evt.params.summary ? `：${evt.params.summary}` : ''}`, {
        level: 'success'
      })
    })
    const offNotice = window.api.cc.onNotice((evt) => {
      pushLog(evt.level, `Stage:${evt.sessionId}`, evt.message)
      showToast(evt.message, { level: evt.level === 'error' ? 'error' : 'warn' })
    })
    const offExit = window.api.cc.onExit((evt) => {
      if (evt.exitCode !== 0 && evt.exitCode !== null) {
        pushLog(
          'warn',
          `Stage:${evt.sessionId}`,
          `CLI 进程退出 code=${evt.exitCode}${evt.signal ? ' signal=' + evt.signal : ''}`
        )
      }
    })
    return () => {
      off()
      offNotice()
      offExit()
    }
  }, [])

  const sessionIdFor = useCallback(
    (stageId: number) => `${currentProjectId ?? 'none'}:stage${stageId}`,
    [currentProjectId]
  )

  /** Ensure the target stage's CC is running; spawn + wait for prompt priming. */
  const ensureStageRunning = useCallback(
    async (stageId: number, sid: string) => {
      if (!currentProjectId) throw new Error('未选择项目')
      const running = await window.api.cc.has(sid)
      if (running) return
      const override = stageConfigs[String(stageId)]
      const res = await window.api.cc.spawn({
        sessionId: sid,
        projectId: currentProjectId,
        stageId,
        projectDir,
        cwd: projectDir,
        label: planName || undefined,
        command: override?.command,
        args: override?.args,
        env: override?.env
      })
      if (!res.ok) throw new Error(`spawn stage ${stageId} failed: ${res.error}`)
      // Wait for system prompt injection + sendMessage to finish.
      // Stage 1 (codex) uses PRIMING_DELAY_MS_STAGE1=2500ms; others 1200ms.
      // sendMessage itself takes time (chunked writes). Wait generously.
      const waitMs = stageId === 1 ? 5000 : 3000
      await new Promise((r) => setTimeout(r, waitMs))
    },
    [projectDir, planName, currentProjectId, stageConfigs]
  )

  /** Load the current Stage 1 plan md and open the review + annotation dialog. */
  const openPlanReview = useCallback(async () => {
    if (!projectDir) return
    const res = await window.api.artifact.readCurrent(
      projectDir,
      1,
      planName || undefined
    )
    if (!res.ok || res.content === undefined) {
      showToast(
        `暂无可预览的方案文件${res.path ? `（${res.path}）` : ''}。请先让 Stage 1 开始对话设计。`,
        { level: 'warn' }
      )
      return
    }
    setPlanReview({ path: res.path ?? '', content: res.content })
  }, [projectDir, planName])

  /** Format annotations as markdown and push them into the Stage 1 session. */
  const submitPlanReviewAnnotations = useCallback(
    async (annotations: Annotation[], generalNote: string) => {
      const sid = sessionIdFor(1)
      const running = await window.api.cc.has(sid)
      if (!running) {
        showToast('Stage 1 的 CLI 未在运行，无法发送标注。请先启动 Stage 1。', {
          level: 'error'
        })
        return
      }
      const lines: string[] = [
        '我查看了当前方案，有以下反馈，请据此修改设计文档后再次输出 <<STAGE_DONE ...>> 标记。',
        ''
      ]
      if (generalNote) {
        lines.push('## 整体意见', '', generalNote, '')
      }
      annotations.forEach((a, i) => {
        lines.push(
          `## 批注 ${i + 1}`,
          '',
          '原文：',
          ...a.quote.split('\n').map((ln) => `> ${ln}`),
          '',
          '意见：',
          a.comment,
          ''
        )
      })
      lines.push(
        '---',
        '',
        '请把以上每条意见落实到方案文件中，修改完成后重新写入产物并打印 <<STAGE_DONE ...>> 完成标记。'
      )
      const res = await window.api.cc.sendUser(sid, lines.join('\n'))
      if (!res.ok) {
        showToast(`发送标注失败：${res.error ?? '未知错误'}`, { level: 'error' })
        return
      }
      showToast(
        `已发送 ${annotations.length} 条批注${generalNote ? ' + 整体意见' : ''}到 Stage 1`,
        { level: 'success' }
      )
      setPlanReview(null)
    },
    [sessionIdFor]
  )

  const advance = useCallback(async () => {
    if (!pendingDone) return
    const next = nextStageFor(pendingDone.stageId, pendingDone.params.verdict, stageSkips)
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
    ? nextStageFor(pendingDone.stageId, pendingDone.params.verdict, stageSkips)
    : null

  return (
    <div className={`app ${pendingDone ? 'has-drawer' : ''}`}>
      <header className="topbar">
        <button
          className="topbar-btn topbar-btn-primary"
          onClick={() => setShowProjectPicker(true)}
          title="管理 / 切换 / 新建项目"
        >
          📁 {hasProject ? `项目：${projectName}` : '选择项目'}
        </button>
        <h1>Multi-AI Code</h1>
        <span className="meta">
          v{version} ·{' '}
          {hasProject ? (
            <code title={targetRepo}>{targetRepo}</code>
          ) : (
            <span className="meta-warn">⚠ 未选择项目（请点左上角「📁 选择项目」）</span>
          )}
        </span>
        <button
          className="topbar-btn"
          onClick={() => setShowStageSettings(true)}
          disabled={!hasProject}
          title="配置每个阶段使用的 CLI 命令 / 参数 / 环境变量"
        >
          ⚙️ 阶段配置
        </button>
        <button
          className="topbar-btn"
          onClick={() => setShowTemplates(true)}
          title="管理与插入常用 prompt 模板"
        >
          📋 模板
        </button>
        <button
          className={`topbar-btn ${errorCount > 0 ? 'topbar-btn-danger' : ''}`}
          onClick={() => setShowErrors((s) => !s)}
          title="查看错误与通知日志"
        >
          {errorCount > 0 ? `⚠ ${errorCount}` : '📣 日志'}
        </button>
        <button
          className="topbar-btn"
          onClick={() => setShowHistory(true)}
          disabled={!hasProject}
          title="查看各阶段产物历史（每次完成自动归档）"
        >
          📋 历史
        </button>
        <button
          className="topbar-btn"
          onClick={() => setShowTimeline(true)}
          disabled={!hasProject}
          title="按时间线回放当前项目的所有阶段事件"
        >
          📜 时间线
        </button>
        <button
          className="topbar-btn"
          onClick={() => setShowOnboarding(true)}
          title="新手上手引导"
        >
          ❓ 向导
        </button>
        <button
          className="topbar-btn"
          onClick={() => setShowDoctor(true)}
          title="检查 claude / codex / git / node 是否就绪"
        >
          🩺 体检
        </button>
        {zoomedStage !== null && (
          <button className="topbar-btn" onClick={() => setZoomedStage(null)}>
            ↙ 退出放大 (Stage {displayIndexOf(zoomedStage) ?? zoomedStage})
          </button>
        )}
      </header>

      {hasProject && planName.trim() && (
        <div className="plan-progress-bar">
          <span className="plan-progress-label">📍 当前方案：</span>
          <strong>{planName}</strong>
          <span className="plan-progress-sep">·</span>
          {STAGES.map((s) => {
            const st = stageStatus[s.id]
            const skipped = stageSkips[s.id]
            const isDone = planStagesDone[s.id]
            let icon = '─'
            let cls = ''
            if (skipped) { icon = '⏭'; cls = 'skipped' }
            else if (st === 'running') { icon = '⏳'; cls = 'running' }
            else if (st === 'awaiting-confirm') { icon = '⏸'; cls = 'awaiting' }
            else if (isDone) { icon = '✅'; cls = 'done' }
            return (
              <span key={s.id} className={`plan-progress-node ${cls}`} title={`${s.name}：${icon}`}>
                <span style={{ marginRight: 4 }}>{'①②③④'[s.id - 1]}</span>
                {icon}
              </span>
            )
          })}
        </div>
      )}

      <div className="main-split">
        <main className={`grid ${zoomedStage !== null ? 'grid-zoomed' : ''}`}>
          {STAGES.map((s, idx) => (
            <StagePanel
              key={s.id}
              stageId={s.id}
              stageName={s.name}
              displayIndex={idx + 1}
              sessionId={sessionIdFor(s.id)}
              projectId={currentProjectId ?? ''}
              projectDir={projectDir}
              cwd={projectDir || '/tmp'}
              zoomed={zoomedStage === s.id}
              hidden={zoomedStage !== null && zoomedStage !== s.id}
              onToggleZoom={() =>
                setZoomedStage((cur) => (cur === s.id ? null : s.id))
              }
              killAllNonce={killAllNonce}
              onStatusChange={handleStatusChange}
              disabled={!hasProject}
              commandOverride={stageConfigs[String(s.id)]?.command}
              args={stageConfigs[String(s.id)]?.args}
              envOverride={stageConfigs[String(s.id)]?.env}
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
              planName={planName}
              onPlanNameChange={s.id === 1 ? setPlanName : undefined}
              planNameSuggestions={planNameSuggestions}
              onPickHistory={
                s.id === 1 ? () => setPickerStage(1) : undefined
              }
              onReviewPlan={s.id === 1 ? openPlanReview : undefined}
              targetRepo={targetRepo}
              msysEnabled={msysEnabled}
            />
          ))}
        </main>

        {showHistory && (
          <HistoryDrawer
            projectId={currentProjectId ?? ''}
            projectDir={projectDir}
            onClose={() => setShowHistory(false)}
            onRestore={async (record) => {
              const res = await window.api.artifact.restore({
                projectId: currentProjectId ?? '',
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
            onMergeViaAI={async (mergedContent) => {
              setShowHistory(false)
              try {
                const sid = sessionIdFor(1)
                const tmpRes = await window.api.writeTemp(mergedContent, 'md')
                if (!tmpRes.ok || !tmpRes.path) {
                  alert(`写入临时文件失败：${tmpRes.error ?? '未知错误'}`)
                  return
                }
                await ensureStageRunning(1, sid)
                const sendRes = await window.api.cc.sendUser(
                  sid,
                  [
                    `请完整读取 ${tmpRes.path}，其中包含用户从历史方案中勾选的多份方案。`,
                    `请进行优化合并：取各方案精华，消除矛盾与冗余，输出一份统一的、完整的设计文档。`,
                    `合并完成后把最终结果写到默认产物路径，并打印 <<STAGE_DONE ...>> 标记。`
                  ].join('\n')
                )
                if (!sendRes.ok) {
                  alert(`发送合并指令失败：${sendRes.error}`)
                }
              } catch (err) {
                alert(`AI 合并优化出错：${(err as Error).message}`)
              }
            }}
          />
        )}
        {pickerStage !== null && (
          <HistoryDrawer
            projectId={currentProjectId ?? ''}
            projectDir={projectDir}
            pickStage={pickerStage}
            onClose={() => setPickerStage(null)}
            onPick={async (snapshotPath) => {
              if (pickerStage === 1 && !planName.trim()) {
                alert('请先在 Stage 1 面板的 "方案名称" 输入框里填写名称，再选用历史方案。')
                return
              }
              const res = await window.api.artifact.restore({
                projectId: currentProjectId ?? '',
                projectDir,
                stageId: pickerStage,
                snapshotPath,
                sessionId: sessionIdFor(pickerStage),
                label: planName
              })
              if (!res.ok) {
                alert(`恢复历史方案失败：${res.error}`)
                return
              }
              setPickerStage(null)
            }}
            onImportFile={async () => {
              if (pickerStage === 1 && !planName.trim()) {
                alert('请先在 Stage 1 面板的 "方案名称" 输入框里填写名称，再从文件导入。')
                return
              }
              // Stage 1 routes through the preview dialog so the user confirms
              // the content before it becomes the stage artifact.
              if (pickerStage === 1) {
                setPickerStage(null)
                void pickExternalFileForPreview(1)
                return
              }
              const res = await window.api.artifact.importFile({
                projectId: currentProjectId ?? '',
                projectDir,
                stageId: pickerStage,
                sessionId: sessionIdFor(pickerStage),
                label: planName
              })
              if (res.canceled) return
              if (!res.ok) {
                alert(`导入文件失败：${res.error}`)
                return
              }
              setPickerStage(null)
            }}
            onRefine={async (snapshotPath) => {
              if (pickerStage === 1 && !planName.trim()) {
                alert('请先在 Stage 1 面板的 "方案名称" 输入框里填写名称，再继续完善此方案。')
                return
              }
              const seeded = await window.api.artifact.seed({
                projectId: currentProjectId ?? '',
                projectDir,
                stageId: pickerStage,
                snapshotPath,
                label: planName
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
            onMergeViaAI={async (mergedContent) => {
              const stage = pickerStage
              setPickerStage(null)
              try {
                const sid = sessionIdFor(stage)
                const tmpRes = await window.api.writeTemp(mergedContent, 'md')
                if (!tmpRes.ok || !tmpRes.path) {
                  alert(`写入临时文件失败：${tmpRes.error ?? '未知错误'}`)
                  return
                }
                await ensureStageRunning(stage, sid)
                const sendRes = await window.api.cc.sendUser(
                  sid,
                  [
                    `请完整读取 ${tmpRes.path}，其中包含用户从历史方案中勾选的多份方案。`,
                    `请进行优化合并：取各方案精华，消除矛盾与冗余，输出一份统一的、完整的设计文档。`,
                    `合并完成后把最终结果写到默认产物路径，并打印 <<STAGE_DONE ...>> 标记。`
                  ].join('\n')
                )
                if (!sendRes.ok) {
                  alert(`发送合并指令失败：${sendRes.error}`)
                }
              } catch (err) {
                alert(`AI 合并优化出错：${(err as Error).message}`)
              }
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
        {showProjectPicker && (
          <ProjectPicker
            currentId={currentProjectId}
            onClose={() => setShowProjectPicker(false)}
            onSelect={(p) => {
              if (p.id === currentProjectId) {
                setShowProjectPicker(false)
                return
              }
              void window.api.cc.killAll()
              setKillAllNonce((n) => n + 1)
              clearProjectScopedState()
              const saved = localStorage.getItem(PLAN_NAME_KEY_PREFIX + p.id)
              if (saved) setPlanName(saved)
              setCurrentProjectId(p.id)
              setShowProjectPicker(false)
            }}
            onChanged={async () => {
              const list = await reloadProjects()
              // If the currently-open project was deleted, bail out of it cleanly.
              if (currentProjectId && !list.some((p) => p.id === currentProjectId)) {
                void window.api.cc.killAll()
                setKillAllNonce((n) => n + 1)
                clearProjectScopedState()
                setCurrentProjectId(null)
                showToast('当前项目已被删除，请另选一个或新建', { level: 'warn' })
              }
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
            targetRepo={targetRepo}
            planName={planName}
          />
        )}
      </div>

      {showGlobalSearch && currentProjectId && (
        <GlobalSearchDialog
          projectId={currentProjectId}
          projectDir={projectDir}
          onClose={() => setShowGlobalSearch(false)}
        />
      )}
      {showCmdk && (
        <CommandPalette
          onClose={() => setShowCmdk(false)}
          commands={[
            { id: 'proj.picker', label: '📁 项目管理（切换 / 新建 / 删除）', keywords: 'project switch new', action: () => setShowProjectPicker(true) },
            { id: 'onboard', label: '❓ 新手向导', keywords: 'help onboarding wizard', action: () => setShowOnboarding(true) },
            { id: 'doctor', label: '🩺 CLI 体检', keywords: 'doctor check health', action: () => setShowDoctor(true) },
            { id: 'settings', label: '⚙️ 阶段 CLI 配置', keywords: 'settings command', action: () => setShowStageSettings(true), disabled: !hasProject },
            { id: 'tpl', label: '📋 Prompt 模板', keywords: 'templates prompt snippets', action: () => setShowTemplates(true) },
            { id: 'hist', label: '📋 产物历史', keywords: 'history artifacts', action: () => setShowHistory(true), disabled: !hasProject },
            { id: 'timeline', label: '📜 审计时间线', keywords: 'timeline events audit', action: () => setShowTimeline(true), disabled: !hasProject },
            { id: 'search', label: '🔍 全局搜索', hint: 'Ctrl+Shift+F', keywords: 'find search', action: () => setShowGlobalSearch(true), disabled: !hasProject },
            { id: 'logs', label: '📣 错误与通知', keywords: 'errors log notifications', action: () => setShowErrors(true) },
            { id: 'unzoom', label: '↙ 退出放大模式', keywords: 'zoom focus exit', action: () => setZoomedStage(null), disabled: zoomedStage === null },
            { id: 'z1', label: '🎯 聚焦 Stage 1 方案设计', hint: 'Ctrl+1', action: () => setZoomedStage(1) },
            { id: 'z2', label: '🎯 聚焦 Stage 2 方案实施', hint: 'Ctrl+2', action: () => setZoomedStage(2) },
            { id: 'z3', label: '🎯 聚焦 Stage 3 方案验收', hint: 'Ctrl+3', action: () => setZoomedStage(3) },
            { id: 'z4', label: '🎯 聚焦 Stage 4 测试验证', hint: 'Ctrl+4', action: () => setZoomedStage(4) }
          ] as Command[]}
        />
      )}
      {showDoctor && <DoctorDialog onClose={() => setShowDoctor(false)} />}
      {previewImport && (
        <FilePreviewDialog
          path={previewImport.path}
          content={previewImport.content}
          title={
            previewImport.stageId === 1
              ? '预览外部方案文件 · Stage 1 方案设计'
              : '预览外部文件'
          }
          confirmLabel="✓ 确认使用此方案"
          onCancel={() => setPreviewImport(null)}
          onConfirm={async () => {
            if (!currentProjectId || !projectDir) {
              setPreviewImport(null)
              return
            }
            const stageId = previewImport.stageId
            const res = await window.api.artifact.commitContent({
              projectId: currentProjectId,
              projectDir,
              stageId,
              content: previewImport.content,
              sourcePath: previewImport.path,
              sessionId: sessionIdFor(stageId),
              label: planName
            })
            setPreviewImport(null)
            if (!res.ok) {
              alert(`导入失败：${res.error}`)
            }
          }}
        />
      )}
      {planReview && (
        <PlanReviewDialog
          path={planReview.path}
          content={planReview.content}
          onClose={() => setPlanReview(null)}
          onSubmit={submitPlanReviewAnnotations}
        />
      )}
      {showOnboarding && (
        <OnboardingWizard
          onClose={() => setShowOnboarding(false)}
          onDone={async ({ projectId, planName: pn }) => {
            setShowOnboarding(false)
            await reloadProjects()
            setCurrentProjectId(projectId)
            setPlanName(pn)
          }}
        />
      )}
      {showTimeline && currentProjectId && (
        <TimelineDrawer
          projectId={currentProjectId}
          onClose={() => setShowTimeline(false)}
        />
      )}
      {showTemplates && (
        <TemplatesDialog
          sessions={Object.entries(stageStatus)
            .filter(([, st]) => st === 'running' || st === 'awaiting-confirm')
            .map(([sid]) => ({
              stageId: Number(sid),
              sessionId: sessionIdFor(Number(sid)),
              name: STAGES.find((x) => x.id === Number(sid))?.name ?? ''
            }))}
          onClose={() => setShowTemplates(false)}
          onInject={(sessionId, text) => {
            void window.api.cc.sendUser(sessionId, text)
          }}
        />
      )}
      {showStageSettings && currentProjectId && (
        <StageSettingsDialog
          projectId={currentProjectId}
          onClose={() => {
            setShowStageSettings(false)
            void window.api.project.getStageConfigs(currentProjectId).then(setStageConfigs)
            void window.api.project.getMsysEnabled(currentProjectId).then(setMsysEnabled)
          }}
        />
      )}
      {showErrors && <ErrorPanel onClose={() => setShowErrors(false)} />}

      <ToastHost />

      <footer className="pipeline">
        <span>Pipeline:</span>
        {STAGES.map((s, i) => (
          <span key={s.id} className={`pipe-node ${stageSkips[s.id] ? 'pipe-skipped' : ''}`}>
            {s.name}
            {i < STAGES.length - 1 && <span className="pipe-arrow">─▶</span>}
          </span>
        ))}
      </footer>
    </div>
  )
}
