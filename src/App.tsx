import { useCallback, useEffect, useRef, useState } from 'react'
import StagePanel from './components/StagePanel'
import CompletionDrawer from './components/CompletionDrawer'
import FeedbackDialog from './components/FeedbackDialog'
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
import DiffViewerDialog, { type DiffAnnotation } from './components/DiffViewerDialog'
import type { StageDoneEvent } from '../electron/preload'

const LAST_PROJECT_KEY = 'multi-ai-code.lastProjectId'

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
  const [planList, setPlanList] = useState<
    { name: string; abs: string; source: 'internal' | 'external' }[]
  >([])
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
  // Stage-3 diff-review state. When true, DiffViewerDialog is rendered.
  const [diffReviewOpen, setDiffReviewOpen] = useState(false)
  // Remember which (project, planName) combos have already seen the starter
  // "import from file?" toast so it's not shown repeatedly on each start.
  const shownStarterHintsRef = useRef<Set<string>>(new Set())

  const stageSkips: Record<number, boolean> = {
    1: !!stageConfigs['1']?.skip,
    2: !!stageConfigs['2']?.skip,
    3: !!stageConfigs['3']?.skip,
    4: !!stageConfigs['4']?.skip
  }
  const [logs] = useLogs()
  const errorCount = logs.filter((l) => l.level === 'error' || l.level === 'warn').length
  const [stageStatus, setStageStatus] = useState<Record<number, string>>({})

  const handleStatusChange = useCallback((stageId: number, status: string) => {
    setStageStatus((prev) =>
      prev[stageId] === status ? prev : { ...prev, [stageId]: status }
    )
  }, [])

  const reloadProjectsRef = useRef<() => Promise<ProjectInfo[]>>(
    async () => []
  )

  const reloadProjects = useCallback(async () => {
    const list = await window.api.project.list()
    setProjects(list)
    return list
  }, [])

  // Sync ref so openProjectDirPicker (declared above reloadProjects for
  // readability) can call the latest reloadProjects without a circular dep.
  useEffect(() => {
    reloadProjectsRef.current = reloadProjects
  }, [reloadProjects])

  /** Clear UI state tied to a specific project (drawers, dialogs, plan name). */
  const clearProjectScopedState = useCallback(() => {
    setPendingDone(null)
    setFeedbackFrom(null)
    setFeedbackForcedTarget(null)
    setShowGlobalSearch(false)
    setZoomedStage(null)
    setPlanName('')
    setPreviewImport(null)
    setPlanReview(null)
    setDiffReviewOpen(false)
    shownStarterHintsRef.current.clear()
  }, [])

  /** One-click "switch project" flow: native directory picker. If the
   *  picked dir is already registered as a project target_repo, switch to
   *  it; otherwise register it as a new project using the dir's basename. */
  const openProjectDirPicker = useCallback(async () => {
    const pick = await window.api.project.pickDir()
    if (pick.canceled || !pick.path) return
    const picked = pick.path
    const existing = projects.find((p) => p.target_repo === picked)
    if (existing) {
      if (existing.id === currentProjectId) return
      void window.api.cc.killAll()
      setKillAllNonce((n) => n + 1)
      clearProjectScopedState()
      setCurrentProjectId(existing.id)
      return
    }
    const basename = picked.split(/[\\/]/).pop()?.trim() || '新项目'
    const res = await window.api.project.create(basename, picked)
    if (!res.ok || !res.id) {
      alert(`创建项目失败：${res.error ?? '未知错误'}`)
      return
    }
    await reloadProjectsRef.current()
    void window.api.cc.killAll()
    setKillAllNonce((n) => n + 1)
    clearProjectScopedState()
    setCurrentProjectId(res.id)
  }, [projects, currentProjectId, clearProjectScopedState])

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

  // Refresh plan list + per-stage "done" status when project/plan changes.
  useEffect(() => {
    if (!currentProjectId || !projectDir) {
      setPlanList([])
      setPlanStagesDone({})
      return
    }
    let cancelled = false
    void (async () => {
      const [planRes, all] = await Promise.all([
        window.api.plan.list(projectDir),
        window.api.artifact.list(currentProjectId)
      ])
      if (cancelled) return
      // On IPC error keep the previous list and current selection untouched —
      // do NOT wipe planName based on a transient failure.
      if (!planRes.ok) return
      const items = planRes.items
      setPlanList(items)
      // Reset planName ONLY when the project actually has plans and the
      // current name is no longer among them. An empty list usually means
      // "brand-new project, user just typed a name via onboarding" — wiping
      // it then would break the onboarding handoff.
      if (planName && items.length > 0 && !items.some((p) => p.name === planName)) {
        setPlanName('')
      }
      const stageSafe = planName
        .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
        .replace(/\s+/g, '_')
        .slice(0, 80)
      const done: Record<number, boolean> = { 1: false, 2: false, 3: false, 4: false }
      for (const r of all) {
        const m = r.path.match(/artifacts[/\\]history[/\\]stage(\d+)[/\\](.+?)(_\d[^.]*)?\.md$/)
        if (m && stageSafe && m[2] === stageSafe) {
          done[Number(m[1])] = true
        }
      }
      setPlanStagesDone(done)
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId, projectDir, pendingDone, planName])

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
      // After Stage 1 completes, surface the archived plan's name back into
      // the dropdown so a "+ 新建方案" flow doesn't leave the sentinel
      // selected. Idempotent: if the user already named the plan, the
      // archived basename equals planName and setPlanName is a no-op.
      if (evt.stageId === 1 && evt.artifactPath) {
        const base = evt.artifactPath
          .split(/[\\/]/)
          .pop()
          ?.replace(/\.md$/i, '')
          ?.trim()
        if (base) setPlanName(base)
      }
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

  const onPlanSelect = useCallback(
    async (value: string) => {
      // Block plan switching while Stage 1 is running. The running CLI's
      // artifact path is baked into its CLAUDE.md at spawn time; a live
      // switch would leave the AI writing to the old target.
      if (value !== planName) {
        const sid = sessionIdFor(1)
        const running = await window.api.cc.has(sid)
        if (running) {
          alert('Stage 1 正在运行，请先停止（Kill）后再切换方案。')
          return
        }
      }
      if (value === '__NEW__') {
        setPlanName('')
        return
      }
      const r = await window.api.artifact.readCurrent(projectDir, 1, value)
      if (!r.ok) {
        // Missing artifact file is a normal state for a freshly listed plan
        // that has not been opened yet — keep the selection but skip the
        // preview rather than alarming the user.
        if (r.error && /ENOENT|no such file|找不到|not.*exist/i.test(r.error)) {
          setPlanName(value)
          return
        }
        // Unexpected error: roll back the selection so we don't end up with
        // planName set to a plan we can't read.
        alert(`读取方案失败：${r.error ?? '未知错误'}`)
        return
      }
      setPlanName(value)
      setPlanReview({ path: r.path ?? value, content: r.content ?? '' })
    },
    [projectDir, planName, sessionIdFor]
  )

  const onImportExternal = useCallback(async () => {
    if (!projectDir) {
      alert('请先打开一个项目')
      return
    }
    // Block import while Stage 1 is running — the running CLI's artifact
    // path is baked at spawn time; swapping the plan under it would write
    // the revisions to the wrong file.
    const sid = sessionIdFor(1)
    const running = await window.api.cc.has(sid)
    if (running) {
      alert('Stage 1 正在运行，请先停止（Kill）后再导入外部方案。')
      return
    }
    const pick = await window.api.dialog.pickTextFile({
      title: '选择要导入的外部方案文件 (.md)'
    })
    if (pick.canceled) return
    if (pick.error || !pick.path) {
      alert(`读取文件失败：${pick.error ?? '未知错误'}`)
      return
    }
    const reg = await window.api.plan.registerExternal({
      projectDir,
      externalPath: pick.path
    })
    if (!reg.ok) {
      alert(`导入失败：${reg.error}`)
      return
    }
    const list = await window.api.plan.list(projectDir)
    if (list.ok) setPlanList(list.items)
    setPlanName(reg.name)
    const cur = await window.api.artifact.readCurrent(projectDir, 1, reg.name)
    if (cur.ok) {
      setPlanReview({ path: cur.path ?? reg.name, content: cur.content ?? '' })
    }
  }, [projectDir, sessionIdFor])

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

  /** Open the Stage 3 git diff viewer. */
  const openDiffReview = useCallback(() => {
    if (!targetRepo) {
      showToast('本项目没有 target_repo 路径，无法打开 Diff 审查', { level: 'warn' })
      return
    }
    setDiffReviewOpen(true)
  }, [targetRepo])

  /** Format diff annotations as markdown and push to the Stage 3 session.
   *  If Stage 3 isn't running yet, spawn it first (user-driven entry). */
  const submitDiffAnnotations = useCallback(
    async (anns: DiffAnnotation[], generalNote: string) => {
      const sid = sessionIdFor(3)
      try {
        await ensureStageRunning(3, sid)
      } catch (err) {
        showToast(`启动 Stage 3 失败：${(err as Error).message}`, { level: 'error' })
        return
      }
      const lines: string[] = [
        '我基于以下 diff 审查结果，请你按每一条批注修改对应代码，然后打印 <<STAGE_DONE ...>> 完成标记。',
        ''
      ]
      if (generalNote) {
        lines.push('## 整体意见', '', generalNote, '')
      }
      anns.forEach((a, i) => {
        lines.push(
          `## 批注 ${i + 1}`,
          '',
          `> 文件：${a.file}`,
          `> 行范围：${a.lineRange}`,
          '',
          '原文：',
          '```',
          a.snippet,
          '```',
          '',
          '意见：',
          a.comment,
          ''
        )
      })
      lines.push(
        '---',
        '',
        '请逐条落实，最后单独一行输出 `<<STAGE_DONE summary="...">>` 标记。'
      )
      const res = await window.api.cc.sendUser(sid, lines.join('\n'))
      if (!res.ok) {
        showToast(`发送批注失败：${res.error ?? '未知错误'}`, { level: 'error' })
        return
      }
      showToast(
        `已发送 ${anns.length} 条批注${generalNote ? ' + 整体意见' : ''}到 Stage 3`,
        { level: 'success' }
      )
      setDiffReviewOpen(false)
    },
    [sessionIdFor, ensureStageRunning]
  )

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
          onClick={() => void openProjectDirPicker()}
          title="浏览一个仓库目录作为当前项目（已注册的目录会直接切回，新目录自动注册）"
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
                s.id === 2 || s.id === 4
                  ? () => {
                      setFeedbackForcedTarget(null)
                      setFeedbackFrom(s.id)
                    }
                  : undefined
              }
              onRequestRedesign={
                s.id === 4
                  ? () => {
                      setFeedbackForcedTarget(1)
                      setFeedbackFrom(s.id)
                    }
                  : undefined
              }
              hideManualDone={s.id === 3}
              planName={planName}
              onPlanSelect={s.id === 1 ? onPlanSelect : undefined}
              planList={planList}
              onImportExternal={s.id === 1 ? onImportExternal : undefined}
              onReviewPlan={s.id === 1 ? openPlanReview : undefined}
              onReviewDiff={s.id === 3 ? openDiffReview : undefined}
              targetRepo={targetRepo}
              msysEnabled={msysEnabled}
            />
          ))}
        </main>

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
            // When importing from file for Stage 1, use the ORIGINAL filename
            // (sans extension) as the plan name / archive filename — the
            // archived md keeps the user's original identity instead of being
            // renamed to whatever is currently in the plan-name input.
            let effectiveLabel = planName
            if (stageId === 1) {
              const origName =
                previewImport.path.split(/[\\/]/).pop() ?? ''
              const fromFile = origName.replace(/\.(md|markdown|txt)$/i, '').trim()
              if (fromFile) {
                effectiveLabel = fromFile
                setPlanName(fromFile)
              }
            }
            const res = await window.api.artifact.commitContent({
              projectId: currentProjectId,
              projectDir,
              stageId,
              content: previewImport.content,
              sourcePath: previewImport.path,
              sessionId: sessionIdFor(stageId),
              label: effectiveLabel
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
      {diffReviewOpen && (
        <DiffViewerDialog
          cwd={targetRepo}
          onClose={() => setDiffReviewOpen(false)}
          onSubmit={submitDiffAnnotations}
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
