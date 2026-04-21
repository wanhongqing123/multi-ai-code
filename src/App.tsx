import { useCallback, useEffect, useRef, useState } from 'react'
import { getTheme, toggleTheme } from './utils/theme.js'
import {
  formatInitialMessage,
  formatAnnotationsForSession,
  planNameToFilename
} from './utils/session-message-format'
import MainPanel from './components/MainPanel'
import ProjectPicker, { type ProjectInfo } from './components/ProjectPicker'
import ErrorPanel, { pushLog, useLogs } from './components/ErrorPanel'
import AiSettingsDialog, { type AiSettings } from './components/AiSettingsDialog'
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

const LAST_PROJECT_KEY = 'multi-ai-code.lastProjectId'

export default function App() {
  const [version, setVersion] = useState<string>('')
  const [projects, setProjects] = useState<ProjectInfo[]>([])
  const [currentProjectId, setCurrentProjectId] = useState<string | null>(null)
  const [showProjectPicker, setShowProjectPicker] = useState(false)
  const [planName, setPlanName] = useState('')
  const [showErrors, setShowErrors] = useState(false)
  const [showAiSettings, setShowAiSettings] = useState(false)
  const [aiSettings, setAiSettings] = useState<AiSettings>({ ai_cli: 'claude' })
  const [showTemplates, setShowTemplates] = useState(false)
  const [showTimeline, setShowTimeline] = useState(false)
  const [showOnboarding, setShowOnboarding] = useState(false)
  const [showDoctor, setShowDoctor] = useState(false)
  const [showCmdk, setShowCmdk] = useState(false)
  const [showGlobalSearch, setShowGlobalSearch] = useState(false)
  const [theme, setThemeState] = useState<'light' | 'dark'>(() => getTheme())

  // Single-stage session state
  const [sessionId, setSessionId] = useState<string | null>(null)
  const [sessionStatus, setSessionStatus] = useState<'idle' | 'running' | 'exited'>('idle')

  const handleToggleTheme = useCallback(() => {
    setThemeState(toggleTheme())
  }, [])

  const [stageConfigs, setStageConfigs] = useState<
    Record<string, { command?: string; args?: string[]; env?: Record<string, string>; skip?: boolean }>
  >({})
  const [planList, setPlanList] = useState<
    { name: string; abs: string; source: 'internal' | 'external' }[]
  >([])
  const [msysEnabled, setMsysEnabled] = useState(false)
  // Stage-1 external-file preview state. When set, FilePreviewDialog is shown
  // and the user can confirm/cancel before the content becomes the stage artifact.
  const [previewImport, setPreviewImport] = useState<{
    path: string
    content: string
    stageId: number
  } | null>(null)
  // Plan-review state. When set, PlanReviewDialog renders the current
  // in-progress plan md; user annotates and the annotations get sent back to
  // the session CLI so it can revise the plan.
  const [planReview, setPlanReview] = useState<{
    path: string
    content: string
  } | null>(null)
  // Diff-review state. When true, DiffViewerDialog is rendered.
  const [diffReviewOpen, setDiffReviewOpen] = useState(false)
  // Remember which (project, planName) combos have already seen the starter
  // "import from file?" toast so it's not shown repeatedly on each start.
  const shownStarterHintsRef = useRef<Set<string>>(new Set())

  const [logs] = useLogs()
  const errorCount = logs.filter((l) => l.level === 'error' || l.level === 'warn').length

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
    setShowGlobalSearch(false)
    setPlanName('')
    setPreviewImport(null)
    setPlanReview(null)
    setDiffReviewOpen(false)
    setSessionId(null)
    setSessionStatus('idle')
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

  // Refresh plan list when project/plan changes.
  useEffect(() => {
    if (!currentProjectId || !projectDir) {
      setPlanList([])
      return
    }
    let cancelled = false
    void (async () => {
      const planRes = await window.api.plan.list(projectDir)
      if (cancelled) return
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
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId, projectDir, planName])

  useEffect(() => {
    if (!currentProjectId) {
      setStageConfigs({})
      setMsysEnabled(false)
      setAiSettings({ ai_cli: 'claude' })
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
    void window.api.project.getAiSettings(currentProjectId).then((settings) => {
      if (cancelled) return
      setAiSettings(settings)
    })
    return () => {
      cancelled = true
    }
  }, [currentProjectId])

  // Wire cc.onExit to flip sessionStatus to 'exited' when the active session exits.
  useEffect(() => {
    const off = window.api.cc.onExit((evt) => {
      if (evt.sessionId === sessionId) {
        setSessionStatus('exited')
      }
    })
    return off
  }, [sessionId])

  useEffect(() => {
    const offNotice = window.api.cc.onNotice((evt) => {
      pushLog(evt.level, `Session:${evt.sessionId}`, evt.message)
      showToast(evt.message, { level: evt.level === 'error' ? 'error' : 'warn' })
    })
    const offExit = window.api.cc.onExit((evt) => {
      if (evt.exitCode !== 0 && evt.exitCode !== null) {
        pushLog(
          'warn',
          `Session:${evt.sessionId}`,
          `CLI 进程退出 code=${evt.exitCode}${evt.signal ? ' signal=' + evt.signal : ''}`
        )
      }
    })
    return () => {
      offNotice()
      offExit()
    }
  }, [])

  // When session starts running with a brand-new plan (not in planList),
  // offer a one-shot import shortcut so the user isn't forced into a fresh design.
  // Shown once per (project, planName) combo per session.
  useEffect(() => {
    if (!currentProjectId || !planName.trim()) return
    if (sessionStatus !== 'running') return
    if (planList.some((p) => p.name === planName.trim())) return
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
  }, [currentProjectId, planName, sessionStatus, planList, pickExternalFileForPreview])

  /** Derive the absolute plan path from planList or construct a default. */
  const getPlanAbsPath = useCallback(
    (name: string): string => {
      const entry = planList.find((p) => p.name === name)
      if (entry) return entry.abs
      // Default: internal design path under target_repo
      return `${targetRepo.replace(/\/$/, '')}/.multi-ai-code/designs/${planNameToFilename(name)}`
    },
    [planList, targetRepo]
  )

  const handleStart = useCallback(async () => {
    if (!currentProjectId || !planName.trim()) return
    const proj = projects.find((p) => p.id === currentProjectId)
    if (!proj?.target_repo) {
      showToast('当前项目未设置 target_repo，请先在项目选择器里选一个代码仓库', { level: 'warn' })
      return
    }
    const planAbsPath = getPlanAbsPath(planName.trim())
    let planContent: string | null = null
    try {
      const res = await window.api.fs.readUtf8(planAbsPath)
      planContent = res.ok ? res.content : null
    } catch {
      planContent = null
    }
    const initialUserMessage = formatInitialMessage({
      planName: planName.trim(),
      planAbsPath,
      planContent
    })
    const command = aiSettings.command ?? aiSettings.ai_cli ?? 'claude'
    const defaultArgs = command === 'codex' ? ['--full-auto'] : []
    const extraArgs = aiSettings.args ?? []
    const args = [...defaultArgs, ...extraArgs]
    const sid = `sess_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`
    setSessionId(sid)
    setSessionStatus('running')
    const currentProj = projects.find((p) => p.id === currentProjectId)
    const pDir = currentProj?.dir ?? ''
    const res = await window.api.cc.spawn({
      sessionId: sid,
      projectId: currentProjectId,
      projectDir: pDir,
      targetRepo: proj.target_repo,
      planName: planName.trim(),
      planAbsPath,
      planPending: planContent === null,
      initialUserMessage,
      command,
      args,
      env: aiSettings.env ?? {}
    })
    if (!res.ok) {
      showToast(res.error ?? '启动失败', { level: 'error' })
      setSessionStatus('idle')
      setSessionId(null)
    }
  }, [currentProjectId, planName, projects, aiSettings, getPlanAbsPath])

  const handleStop = useCallback(async () => {
    if (!sessionId) return
    await window.api.cc.kill(sessionId)
    setSessionStatus('exited')
  }, [sessionId])

  const handleRestart = useCallback(async () => {
    if (sessionId) {
      await window.api.cc.kill(sessionId)
    }
    setSessionId(null)
    setSessionStatus('idle')
    setTimeout(() => void handleStart(), 50)
  }, [sessionId, handleStart])

  const onPlanSelect = useCallback(
    async (value: string) => {
      // Block plan switching while session is running. The running CLI's
      // artifact path is baked at spawn time; a live switch would leave
      // the AI writing to the old target.
      if (value !== planName && sessionStatus === 'running') {
        alert('会话正在运行，请先停止（Kill）后再切换方案。')
        return
      }
      if (value === '__NEW__') {
        setPlanName('')
        return
      }
      const r = await window.api.artifact.readCurrent(projectDir, 1, value)
      if (!r.ok) {
        if (r.error && /ENOENT|no such file|找不到|not.*exist/i.test(r.error)) {
          setPlanName(value)
          return
        }
        alert(`读取方案失败：${r.error ?? '未知错误'}`)
        return
      }
      setPlanName(value)
      setPlanReview({ path: r.path ?? value, content: r.content ?? '' })
    },
    [projectDir, planName, sessionStatus]
  )

  const onImportExternal = useCallback(async () => {
    if (!projectDir) {
      alert('请先打开一个项目')
      return
    }
    // Block import while session is running — the running CLI's artifact
    // path is baked at spawn time; swapping the plan under it would write
    // the revisions to the wrong file.
    if (sessionStatus === 'running') {
      alert('会话正在运行，请先停止（Kill）后再导入外部方案。')
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
  }, [projectDir, sessionStatus])

  /** Load the current plan md and open the review + annotation dialog. */
  const openPlanReview = useCallback(async () => {
    if (!projectDir) return
    const res = await window.api.artifact.readCurrent(
      projectDir,
      1,
      planName || undefined
    )
    if (!res.ok || res.content === undefined) {
      showToast(
        `暂无可预览的方案文件${res.path ? `（${res.path}）` : ''}。请先让 AI 开始对话设计。`,
        { level: 'warn' }
      )
      return
    }
    setPlanReview({ path: res.path ?? '', content: res.content })
  }, [projectDir, planName])

  /** Open the git diff viewer. */
  const openDiffReview = useCallback(() => {
    if (!targetRepo) {
      showToast('本项目没有 target_repo 路径，无法打开 Diff 审查', { level: 'warn' })
      return
    }
    setDiffReviewOpen(true)
  }, [targetRepo])

  /** Format annotations as markdown and push them into the running session. */
  const submitPlanReviewAnnotations = useCallback(
    async (annotations: Annotation[], generalNote: string) => {
      if (!sessionId) {
        showToast('会话的 CLI 未在运行，无法发送标注。请先启动会话。', {
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
      const res = await window.api.cc.sendUser(sessionId, lines.join('\n'))
      if (!res.ok) {
        showToast(`发送标注失败：${res.error ?? '未知错误'}`, { level: 'error' })
        return
      }
      showToast(
        `已发送 ${annotations.length} 条批注${generalNote ? ' + 整体意见' : ''}到会话`,
        { level: 'success' }
      )
      setPlanReview(null)
    },
    [sessionId]
  )

  /** Format diff annotations using session-message-format and push to live session via cc.write. */
  const submitDiffAnnotations = useCallback(
    async (anns: DiffAnnotation[], generalNote: string) => {
      if (!sessionId || sessionStatus !== 'running') {
        showToast('会话未启动，无法发送批注', { level: 'warn' })
        return
      }
      const planAbsPath = getPlanAbsPath(planName.trim())
      const text = formatAnnotationsForSession({
        annotations: anns,
        generalComment: generalNote,
        planAbsPath
      })
      window.api.cc.write(sessionId, text + '\r')
      setDiffReviewOpen(false)
      showToast(`已发送 ${anns.length} 条批注到会话`, { level: 'info' })
    },
    [sessionId, sessionStatus, planName, getPlanAbsPath]
  )

  // Suppress unused warning — onImportExternal and openPlanReview are used
  // in command palette and plan-review flow.
  void onImportExternal
  void openPlanReview
  void openDiffReview

  return (
    <div className="app">
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
          onClick={() => setShowAiSettings(true)}
          disabled={!hasProject}
          title="配置 AI CLI 命令 / 参数 / 环境变量"
        >
          ⚙️ AI 设置
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
        <button
          className="topbar-btn"
          onClick={handleToggleTheme}
          title={theme === 'dark' ? '切换到浅色' : '切换到暗色'}
          aria-label="切换主题"
        >
          {theme === 'dark' ? '☀' : '☾'}
        </button>
      </header>

      {hasProject && planName.trim() && (
        <div className="plan-progress-bar">
          <span className="plan-progress-label">📍 当前方案：</span>
          <strong>{planName}</strong>
          <span className="plan-progress-sep">·</span>
          <span
            className={`plan-progress-node ${sessionStatus === 'running' ? 'running' : sessionStatus === 'exited' ? 'done' : ''}`}
            title={`会话状态：${sessionStatus}`}
          >
            {sessionStatus === 'running' ? '⏳ 运行中' : sessionStatus === 'exited' ? '✅ 已完成' : '─ 未启动'}
          </span>
        </div>
      )}

      <div className="main-split">
        <MainPanel
          sessionId={sessionId ?? ''}
          projectId={currentProjectId ?? ''}
          projectDir={projectDir}
          cwd={targetRepo}
          planName={planName}
          status={sessionStatus}
          onStart={handleStart}
          onStop={handleStop}
          onRestart={handleRestart}
          onOpenDiff={() => setDiffReviewOpen(true)}
          disabled={!currentProjectId || !planName.trim()}
        />

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
              clearProjectScopedState()
              setCurrentProjectId(p.id)
              setShowProjectPicker(false)
            }}
            onChanged={async () => {
              const list = await reloadProjects()
              // If the currently-open project was deleted, bail out of it cleanly.
              if (currentProjectId && !list.some((p) => p.id === currentProjectId)) {
                void window.api.cc.killAll()
                clearProjectScopedState()
                setCurrentProjectId(null)
                showToast('当前项目已被删除，请另选一个或新建', { level: 'warn' })
              }
            }}
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
            { id: 'settings', label: '⚙️ AI 设置', keywords: 'settings command ai cli', action: () => setShowAiSettings(true), disabled: !hasProject },
            { id: 'tpl', label: '📋 Prompt 模板', keywords: 'templates prompt snippets', action: () => setShowTemplates(true) },
            { id: 'timeline', label: '📜 审计时间线', keywords: 'timeline events audit', action: () => setShowTimeline(true), disabled: !hasProject },
            { id: 'search', label: '🔍 全局搜索', hint: 'Ctrl+Shift+F', keywords: 'find search', action: () => setShowGlobalSearch(true), disabled: !hasProject },
            { id: 'logs', label: '📣 错误与通知', keywords: 'errors log notifications', action: () => setShowErrors(true) },
            { id: 'toggle-theme', label: theme === 'dark' ? '切换到浅色主题' : '切换到暗色主题', keywords: 'theme dark light color', action: handleToggleTheme },
            { id: 'plan-review', label: '📝 审阅当前方案', keywords: 'plan review annotate', action: () => void openPlanReview(), disabled: !hasProject || !planName.trim() },
            { id: 'diff-review', label: '🔀 Diff 审查', keywords: 'diff review code', action: () => void openDiffReview(), disabled: !hasProject }
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
              ? '预览外部方案文件 · 方案设计'
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
            // When importing from file, use the ORIGINAL filename
            // (sans extension) as the plan name / archive filename.
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
              sessionId: sessionId ?? undefined,
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
          sessionRunning={sessionStatus === 'running'}
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
          sessions={
            sessionId && sessionStatus === 'running'
              ? [{ stageId: 1, sessionId, name: '当前会话' }]
              : []
          }
          onClose={() => setShowTemplates(false)}
          onInject={(sid, text) => {
            void window.api.cc.sendUser(sid, text)
          }}
        />
      )}
      {showAiSettings && currentProjectId && (
        <AiSettingsDialog
          projectId={currentProjectId}
          initial={aiSettings}
          onClose={() => setShowAiSettings(false)}
          onSaved={(next) => setAiSettings(next)}
        />
      )}
      {showErrors && <ErrorPanel onClose={() => setShowErrors(false)} />}

      <ToastHost />

      <footer className="pipeline">
        <span>Multi-AI Code · 单阶段架构</span>
        {hasProject && (
          <span style={{ marginLeft: 8, opacity: 0.7 }}>
            {sessionStatus === 'running'
              ? '● 运行中'
              : sessionStatus === 'exited'
              ? '○ 已退出'
              : '○ 空闲'}
          </span>
        )}
      </footer>
    </div>
  )
}
