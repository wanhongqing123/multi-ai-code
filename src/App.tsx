import { useCallback, useEffect, useRef, useState } from 'react'
import { getTheme, toggleTheme } from './utils/theme.js'
import {
  formatInitialMessage,
  formatAnnotationsForSession,
  planNameToFilename
} from './utils/session-message-format'
import { buildCliLaunchArgs } from './utils/cliLaunchArgs'
import MainPanel from './components/MainPanel'
import MainBootGate, { type BootGatePhase } from './components/MainBootGate'
import ProjectPicker, { type ProjectInfo } from './components/ProjectPicker'
import ErrorPanel, { pushLog, useLogs } from './components/ErrorPanel'
import AiSettingsDialog, {
  type AiSettings,
  type AppSettings
} from './components/AiSettingsDialog'
import ProjectBuildPanel, { getBuildStartBlockedReason } from './components/ProjectBuildPanel'
import TemplatesDialog from './components/TemplatesDialog'
import HabitMonitorDialog from './habit/HabitMonitorDialog'
import FirstRunNoticeDialog from './habit/FirstRunNoticeDialog'
import { getCliTargetLabel } from './components/cliTarget'
import OnboardingWizard from './components/OnboardingWizard'
import DoctorDialog from './components/DoctorDialog'
import CommandPalette, { type Command } from './components/CommandPalette'
import ToastHost, { showToast } from './components/Toast'
import GlobalSearchDialog from './components/GlobalSearchDialog'
import FilePreviewDialog from './components/FilePreviewDialog'
import PlanReviewDialog, { type Annotation } from './components/PlanReviewDialog'
import DiffViewerDialog, { type DiffAnnotation } from './components/DiffViewerDialog'
import type { DiffMode } from './components/diffViewerConfig'
import type { ExternalReviewSuggestion } from './components/externalAiReview'
import type {
  BuildRuntimeState,
  ManagedChromeState,
  ProjectBuildConfig,
  VisualStudioInstallation
} from '../electron/preload'
import type { HabitFlowRow, HabitSettings } from './habit/habitTypes'

const LAST_PROJECT_KEY = 'multi-ai-code.lastProjectId'
const DEFAULT_PROJECT_BUILD_CONFIG: ProjectBuildConfig = { enabled: false, steps: [] }
const BUILD_LOG_LIMIT = 200_000
const DEFAULT_BUILD_RUNTIME_STATE: BuildRuntimeState = {
  status: 'idle',
  scope: null,
  requestedStepId: null,
  projectId: null,
  projectName: null,
  targetRepo: null,
  startedAt: null,
  finishedAt: null,
  activeStepId: null,
  steps: [],
  log: '',
  lastFailure: null
}

function appendBuildLog(current: string, chunk: string): string {
  const next = current + chunk
  if (next.length <= BUILD_LOG_LIMIT) return next
  return `...[build log truncated]...\n${next.slice(-BUILD_LOG_LIMIT)}`
}

const DEFAULT_MANAGED_CHROME_STATE: ManagedChromeState = {
  running: false,
  port: null,
  profileDir: null,
  pid: null,
  lastActiveUrl: null
}

function parseHabitFlowAction(flow: HabitFlowRow): string | null {
  try {
    const payload = JSON.parse(flow.payload) as { action?: unknown }
    return typeof payload.action === 'string' ? payload.action : null
  } catch {
    return null
  }
}

export function deriveHabitUiFlags(input: {
  autoPersonalizeUi: boolean
  flows: HabitFlowRow[]
}): { hideTemplatesEntry: boolean; hideWizardEntry: boolean } {
  if (!input.autoPersonalizeUi) {
    return {
      hideTemplatesEntry: false,
      hideWizardEntry: false
    }
  }

  const actions = new Set(
    input.flows
      .filter((flow) => flow.status === 'active' && flow.kind === 'ui-adjustment')
      .map(parseHabitFlowAction)
      .filter((action): action is string => typeof action === 'string')
  )

  return {
    hideTemplatesEntry: actions.has('hide-templates-entry') || input.autoPersonalizeUi,
    hideWizardEntry: actions.has('hide-wizard-entry') || input.autoPersonalizeUi
  }
}

export function shouldRenderElectronShell(): boolean {
  return typeof window === 'undefined' || typeof window.api !== 'undefined'
}

function ElectronLaunchRequired(): JSX.Element {
  return (
    <div className="app">
      <div
        className="drawer-empty"
        style={{ maxWidth: 560, margin: '96px auto', padding: '24px' }}
      >
        <strong>{'请通过 Electron 启动'}</strong>
        <div style={{ marginTop: 8 }}>
          {
            '当前页面缺少 preload 注入的桌面 API，只能用于静态预览。请运行 npm run dev 或正式桌面应用。'
          }
        </div>
      </div>
    </div>
  )
}

export default function App(): JSX.Element {
  if (!shouldRenderElectronShell()) {
    return <ElectronLaunchRequired />
  }

  return <AppShell />
}

function AppShell() {
  const [version, setVersion] = useState<string>('')
  const [projects, setProjects] = useState<ProjectInfo[]>([])
  const [currentProjectId, setCurrentProjectId] = useState<string | null>(null)
  const [showProjectPicker, setShowProjectPicker] = useState(false)
  const [planName, setPlanName] = useState('')
  const [showErrors, setShowErrors] = useState(false)
  const [showAiSettings, setShowAiSettings] = useState(false)
  const [aiSettings, setAiSettings] = useState<AiSettings>({ ai_cli: 'claude' })
  const [repoViewAiSettings, setRepoViewAiSettings] = useState<AiSettings>({
    ai_cli: 'claude'
  })
  const [aiSettingsReady, setAiSettingsReady] = useState(false)
  const [aiSettingsLoadError, setAiSettingsLoadError] = useState<string | null>(null)
  const [appSettings, setAppSettings] = useState<AppSettings>({
    screenshotShortcutEnabled: true,
    screenshotShortcut: 'CommandOrControl+Shift+A'
  })
  const [projectBuildConfig, setProjectBuildConfig] = useState<ProjectBuildConfig>(
    DEFAULT_PROJECT_BUILD_CONFIG
  )
  const [projectBuildConfigProjectId, setProjectBuildConfigProjectId] = useState<string | null>(
    null
  )
  const [visualStudioInstallations, setVisualStudioInstallations] = useState<
    VisualStudioInstallation[]
  >([])
  const [visualStudioInstallationsLoading, setVisualStudioInstallationsLoading] =
    useState(false)
  const [showTemplates, setShowTemplates] = useState(false)
  const [showHabitMonitor, setShowHabitMonitor] = useState(false)
  const [showHabitFirstRun, setShowHabitFirstRun] = useState(false)
  const [showOnboarding, setShowOnboarding] = useState(false)
  const [showDoctor, setShowDoctor] = useState(false)
  const [showCmdk, setShowCmdk] = useState(false)
  const [showGlobalSearch, setShowGlobalSearch] = useState(false)
  const [showBuildPanel, setShowBuildPanel] = useState(false)
  const [theme, setThemeState] = useState<'light' | 'dark'>(() => getTheme())
  const [habitSettingsSnapshot, setHabitSettingsSnapshot] = useState<HabitSettings | null>(null)
  const [habitFlowsSnapshot, setHabitFlowsSnapshot] = useState<HabitFlowRow[]>([])
  const [managedChromeState, setManagedChromeState] = useState<ManagedChromeState>(
    DEFAULT_MANAGED_CHROME_STATE
  )
  const [managedChromeBusy, setManagedChromeBusy] = useState(false)

  const visibleProjectBuildConfig =
    currentProjectId !== null && projectBuildConfigProjectId === currentProjectId
      ? projectBuildConfig
      : DEFAULT_PROJECT_BUILD_CONFIG
  const projectBuildConfigReady =
    currentProjectId !== null && projectBuildConfigProjectId === currentProjectId

  // Single-stage session state
  const [sessionId, setSessionId] = useState<string | null>(null)
  const [sessionStatus, setSessionStatus] = useState<'idle' | 'running' | 'exited'>('idle')
  // MainPanel is mounted (true) or the boot gate is shown (false). Toggled
  // by spawn success (→ true) and by "重置主会话" / resume-failure (→ false).
  // sessionStatus === 'exited' deliberately keeps mainPanelMounted=true so
  // the previous terminal scrollback and the existing 重启 button still work.
  const [mainPanelMounted, setMainPanelMounted] = useState(false)
  // Gate's internal display state when mainPanelMounted === false.
  const [gatePhase, setGatePhase] = useState<BootGatePhase>({ kind: 'idle' })

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
  // Lifted from DiffViewerDialog so unsent batches survive a close/reopen
  // cycle. Cleared on: successful submit, project switch, plan switch.
  const [diffAnnotations, setDiffAnnotations] = useState<DiffAnnotation[]>([])
  const [diffGeneralNote, setDiffGeneralNote] = useState('')
  // Diff dialog view state — mode tab + chosen commit + chosen file. Lifted
  // here so closing and reopening the dialog against the same repo preserves
  // where the user was. Cleared on project switch (see clearProjectScopedState).
  const [diffMode, setDiffMode] = useState<DiffMode>('working')
  const [diffSelectedCommit, setDiffSelectedCommit] = useState('')
  const [diffSelectedFile, setDiffSelectedFile] = useState('')
  const [buildState, setBuildState] = useState<BuildRuntimeState>(DEFAULT_BUILD_RUNTIME_STATE)
  // Remember which (project, planName) combos have already seen the starter
  // "import from file?" toast so it's not shown repeatedly on each start.
  const shownStarterHintsRef = useRef<Set<string>>(new Set())

  const [logs] = useLogs()
  const errorCount = logs.filter((l) => l.level === 'error' || l.level === 'warn').length
  const habitUiFlags = deriveHabitUiFlags({
    autoPersonalizeUi: habitSettingsSnapshot?.autoPersonalizeUi ?? true,
    flows: habitFlowsSnapshot
  })

  const reloadProjectsRef = useRef<() => Promise<ProjectInfo[]>>(
    async () => []
  )

  const refreshVisualStudioInstallations = useCallback(async () => {
    setVisualStudioInstallationsLoading(true)
    try {
      const result = await window.api.project.listVisualStudioInstallations()
      if (!result.ok) {
        setVisualStudioInstallations([])
        showToast(result.error ?? '读取 Visual Studio 实例失败', { level: 'error' })
        return
      }
      setVisualStudioInstallations(result.value ?? [])
    } finally {
      setVisualStudioInstallationsLoading(false)
    }
  }, [])

  const refreshHabitMonitorSnapshot = useCallback(async () => {
    try {
      const [settings, flows, chromeState] = await Promise.all([
        window.api.habit.settings.get() as Promise<HabitSettings>,
        window.api.habit.flows.list({
          statuses: ['active', 'candidate', 'disabled'],
          limit: 200
        }) as Promise<HabitFlowRow[]>,
        window.api.habit.chrome.getState() as Promise<ManagedChromeState>
      ])
      setHabitSettingsSnapshot(settings)
      setHabitFlowsSnapshot(flows)
      setManagedChromeState(chromeState)
    } catch {
      /* ignore */
    }
  }, [])

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

  // Show the first-run notice for the habit monitor exactly once per
  // install: settings.firstRunNoticeShownAt is 0 before the user has seen it.
  useEffect(() => {
    void (async () => {
      try {
        const s = (await window.api.habit.settings.get()) as {
          firstRunNoticeShownAt?: number
        }
        if (!s.firstRunNoticeShownAt || s.firstRunNoticeShownAt <= 0) {
          setShowHabitFirstRun(true)
        }
      } catch {
        /* ignore */
      }
    })()
    void refreshHabitMonitorSnapshot()
  }, [refreshHabitMonitorSnapshot])

  // Screenshot delivery: when the editor finishes and main saves the image,
  // it broadcasts {path, prompt} here. Forward to the current main-session
  // CLI exactly the way TemplatesDialog injects: cc.sendUser with the prompt
  // text prefixed with an image path the CLI can read.
  useEffect(() => {
    const off = window.api.screenshot.onDeliver(({ path, prompt }) => {
      if (!sessionId || sessionStatus !== 'running') {
        showToast(
          `截图已保存到 ${path}，但当前会话未启动，请先启动会话再发送`,
          { level: 'warn', duration: 4500 }
        )
        return
      }
      const trimmed = (prompt ?? '').trim()
      const text = trimmed ? `${trimmed}\n图片: ${path}` : `图片: ${path}`
      void window.api.cc.sendUser(sessionId, text)
      showToast('截图已发送到主会话', { level: 'success' })
    })
    const offErr = window.api.screenshot.onError(({ message }) => {
      showToast(message, { level: 'error' })
    })
    return () => {
      off()
      offErr()
    }
  }, [sessionId, sessionStatus])

  useEffect(() => {
    let cancelled = false
    void window.api.build.getState().then((state) => {
      if (cancelled) return
      setBuildState(state)
    })
    const offStatus = window.api.build.onStatus((state) => {
      if (cancelled) return
      setBuildState(state)
    })
    const offData = window.api.build.onData((event) => {
      if (cancelled || !event.chunk) return
      setBuildState((prev) => ({
        ...prev,
        projectId: event.projectId ?? prev.projectId,
        activeStepId: event.stepId ?? prev.activeStepId,
        log: appendBuildLog(prev.log, event.chunk)
      }))
    })
    return () => {
      cancelled = true
      offStatus()
      offData()
    }
  }, [])

  useEffect(() => {
    if (!showAiSettings || !currentProjectId) return
    void refreshVisualStudioInstallations()
  }, [showAiSettings, currentProjectId, refreshVisualStudioInstallations])

  /** Clear UI state tied to a specific project (drawers, dialogs, plan name). */
  const clearProjectScopedState = useCallback(() => {
    setShowGlobalSearch(false)
    setShowBuildPanel(false)
    setPlanName('')
    setPreviewImport(null)
    setPlanReview(null)
    setDiffReviewOpen(false)
    setDiffAnnotations([])
    setDiffGeneralNote('')
    setDiffMode('working')
    setDiffSelectedCommit('')
    setDiffSelectedFile('')
    setSessionId(null)
    setSessionStatus('idle')
    setMainPanelMounted(false)
    setGatePhase({ kind: 'idle' })
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

  const handleManagedChromeAction = useCallback(async () => {
    setManagedChromeBusy(true)
    try {
      if (managedChromeState.running) {
        const result = await window.api.habit.chrome.focus()
        if (!result.ok) {
          showToast(result.error, { level: 'error' })
        }
      } else {
        const result = await window.api.habit.chrome.start()
        if (!result.ok) {
          showToast(result.error, { level: 'error' })
        } else if (result.value) {
          setManagedChromeState(result.value)
        }
      }
      await refreshHabitMonitorSnapshot()
    } finally {
      setManagedChromeBusy(false)
    }
  }, [managedChromeState.running, refreshHabitMonitorSnapshot])

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
    void window.api.settings.getAppSettings().then(setAppSettings)
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
  const buildStateForCurrentProject =
    currentProjectId !== null && buildState.projectId === currentProjectId
      ? buildState
      : DEFAULT_BUILD_RUNTIME_STATE
  const hasProject = currentProject !== null

  // Track plan names we've ever observed in the list. If `planName` was
  // in a previous list but is gone from the current one, that's a
  // deleted/pruned plan and we should clear the selection. A brand-new
  // name (user typing "+ 新建方案") was never in any previous list and
  // should NOT be cleared.
  const knownPlanNamesRef = useRef<Set<string>>(new Set())

  // Refresh plan list when project/projectDir changes. Do NOT depend on
  // planName — that would re-run on every keystroke of the "new plan"
  // input. Use functional setState so we can still prune a stale
  // planName (after switching projects or after the backend auto-prunes
  // a dead external mapping) without reading planName directly.
  useEffect(() => {
    if (!currentProjectId || !projectDir) {
      setPlanList([])
      knownPlanNamesRef.current = new Set()
      return
    }
    let cancelled = false
    void (async () => {
      const planRes = await window.api.plan.list(projectDir)
      if (cancelled) return
      if (!planRes.ok) return
      const items = planRes.items
      setPlanList(items)
      const currentNames = new Set(items.map((p) => p.name))
      setPlanName((prev) => {
        if (!prev) return prev
        if (currentNames.has(prev)) return prev
        // Not in current list — was it in a previous list?
        if (knownPlanNamesRef.current.has(prev)) {
          // Was known, now gone → stale selection, clear it.
          return ''
        }
        // Never seen → user is typing a new plan name, keep it.
        return prev
      })
      // Update "ever seen" set AFTER the prune check.
      for (const n of currentNames) knownPlanNamesRef.current.add(n)
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId, projectDir])

  useEffect(() => {
    if (!currentProjectId) {
      setStageConfigs({})
      setMsysEnabled(false)
      setAiSettings({ ai_cli: 'claude' })
      setRepoViewAiSettings({ ai_cli: 'claude' })
      setProjectBuildConfig(DEFAULT_PROJECT_BUILD_CONFIG)
      setProjectBuildConfigProjectId(null)
      setAiSettingsReady(false)
      setAiSettingsLoadError(null)
      return
    }
    localStorage.setItem(LAST_PROJECT_KEY, currentProjectId)
    void window.api.project.touch(currentProjectId)
    let cancelled = false
    setProjectBuildConfig(DEFAULT_PROJECT_BUILD_CONFIG)
    setProjectBuildConfigProjectId(null)
    setAiSettingsReady(false)
    setAiSettingsLoadError(null)
    void window.api.project.getStageConfigs(currentProjectId).then((cfg) => {
      if (cancelled) return
      setStageConfigs(cfg)
    })
    void window.api.project.getMsysEnabled(currentProjectId).then((enabled) => {
      if (cancelled) return
      setMsysEnabled(enabled)
    })
    void (async () => {
      const buildResult = await window.api.project.getBuildConfig(currentProjectId)
      if (cancelled) return
      if (!buildResult.ok) {
        setProjectBuildConfig(DEFAULT_PROJECT_BUILD_CONFIG)
        setProjectBuildConfigProjectId(currentProjectId)
        showToast(buildResult.error ?? '读取项目构建配置失败', { level: 'error' })
      } else {
        setProjectBuildConfig(buildResult.value ?? DEFAULT_PROJECT_BUILD_CONFIG)
        setProjectBuildConfigProjectId(currentProjectId)
      }
      const buildConfigRepaired = buildResult.ok && buildResult.repaired === true

      const aiResult = await window.api.project.getAiSettings(currentProjectId)
      if (cancelled) return
      if (!aiResult.ok) {
        setAiSettingsReady(false)
        setAiSettingsLoadError(aiResult.error ?? '读取主会话 AI 设置失败')
        showToast(aiResult.error ?? '读取主会话 AI 设置失败', { level: 'error' })
        return
      }
      setAiSettings(aiResult.value ?? { ai_cli: 'claude' })
      setAiSettingsReady(true)
      setAiSettingsLoadError(null)

      const repoResult = await window.api.project.getRepoViewAiSettings(currentProjectId)
      if (cancelled) return
      if (!repoResult.ok) {
        showToast(repoResult.error ?? '读取仓库查看 AI 设置失败', { level: 'error' })
        return
      }
      setRepoViewAiSettings(repoResult.value ?? { ai_cli: 'claude' })

      if (aiResult.repaired || repoResult.repaired || buildConfigRepaired) {
        showToast('项目设置文件已自动修复', { level: 'success' })
      }
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId])

  useEffect(() => {
    void refreshHabitMonitorSnapshot()
  }, [currentProjectId, refreshHabitMonitorSnapshot])

  // Wire cc.onExit to flip sessionStatus to 'exited' when the active session exits.
  useEffect(() => {
    const off = window.api.cc.onExit((evt) => {
      if (evt.sessionId === sessionId) {
        setSessionStatus('exited')
      }
    })
    return off
  }, [sessionId])

  // Resume-mode failure: CLI exited within the 5s window with a non-zero
  // code. Return to the boot gate with the error visible and let the user
  // pick again.
  useEffect(() => {
    const off = window.api.cc.onResumeFailed((evt) => {
      if (evt.sessionId !== sessionId) return
      setGatePhase({
        kind: 'failed',
        reason: `CLI 启动后退出 (code=${evt.exitCode})`,
        tail: evt.tail
      })
      setMainPanelMounted(false)
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

  const handleStart = useCallback(async (mode: 'new' | 'resume' = 'new') => {
    if (!currentProjectId || !planName.trim()) return
    if (!aiSettingsReady) {
      showToast(aiSettingsLoadError ?? '主会话 AI 设置尚未加载完成', { level: 'warn' })
      return
    }
    if (aiSettingsLoadError) {
      showToast(aiSettingsLoadError, { level: 'error' })
      return
    }
    const proj = projects.find((p) => p.id === currentProjectId)
    if (!proj?.target_repo) {
      showToast('当前项目未设置 target_repo，请先在项目选择器里选一个代码仓库', { level: 'warn' })
      return
    }
    setGatePhase({ kind: 'spawning', mode })
    const normalizedPlanName = planName.trim()
    const planAbsPath = getPlanAbsPath(normalizedPlanName)
    const planExists = planList.some((p) => p.name === normalizedPlanName)
    const initialUserMessage = formatInitialMessage({
      planName: normalizedPlanName,
      planAbsPath,
      planExists
    })
    const command = aiSettings.command ?? aiSettings.ai_cli ?? 'claude'
    const args = buildCliLaunchArgs(
      aiSettings.ai_cli ?? 'claude',
      proj.target_repo,
      aiSettings.args ?? []
    )
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
      planName: normalizedPlanName,
      planAbsPath,
      planPending: !planExists,
      initialUserMessage,
      command,
      args,
      env: aiSettings.env ?? {},
      mode
    })
    if (!res.ok) {
      showToast(res.error ?? '启动失败', { level: 'error' })
      setSessionStatus('idle')
      setSessionId(null)
      setGatePhase({ kind: 'idle' })
      setMainPanelMounted(false)
      return
    }
    setMainPanelMounted(true)
  }, [currentProjectId, planName, planList, projects, aiSettings, aiSettingsReady, aiSettingsLoadError, getPlanAbsPath])

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

  const handleStartBuild = useCallback(async () => {
    if (
      buildState.projectId !== null &&
      buildState.projectId !== currentProjectId &&
      buildState.status === 'running'
    ) {
      showToast('另一个项目的构建仍在运行，请先停止后再启动当前项目构建', {
        level: 'warn'
      })
      setShowBuildPanel(true)
      return
    }
    const blockedReason = getBuildStartBlockedReason(
      currentProjectId,
      projectBuildConfigReady,
      visibleProjectBuildConfig
    )
    if (blockedReason) {
      showToast(blockedReason, { level: 'warn' })
      setShowBuildPanel(true)
      return
    }
    if (!currentProjectId) return

    const result = await window.api.build.start(currentProjectId)
    setBuildState(result.state)
    setShowBuildPanel(true)
    if (!result.ok) {
      showToast(result.error ?? '启动构建失败', { level: 'error' })
    }
  }, [currentProjectId, projectBuildConfigReady, visibleProjectBuildConfig])

  const handleStopBuild = useCallback(async () => {
    const result = await window.api.build.stop()
    if (!result.ok) {
      showToast(result.error ?? '停止构建失败', { level: 'error' })
    }
  }, [])

  const handleAnalyzeBuildFailure = useCallback(async () => {
    if (buildState.status !== 'failed' || !buildState.lastFailure) {
      showToast('当前没有可分析的构建失败上下文', { level: 'warn' })
      return
    }
    if (!currentProjectId || buildState.projectId !== currentProjectId) {
      showToast('当前失败上下文不属于所选项目，请切回对应项目后再分析', {
        level: 'warn'
      })
      return
    }
    if (!sessionId || sessionStatus !== 'running') {
      showToast('主会话未运行，无法发送失败分析请求，请先启动主会话', { level: 'warn' })
      return
    }

    const promptResult = await window.api.build.getFailureAnalysisPrompt()
    if (!promptResult.ok) {
      showToast(promptResult.error ?? '获取失败分析提示失败', { level: 'error' })
      return
    }

    const sendResult = await window.api.cc.sendUser(sessionId, promptResult.prompt)
    if (!sendResult.ok) {
      showToast(sendResult.error ?? '发送失败分析请求失败', { level: 'error' })
      return
    }

    showToast('已将构建失败原因分析请求发送到主会话', { level: 'success' })
  }, [buildState, currentProjectId, sessionId, sessionStatus])

  /**
   * Kill the current main session (if any) and return the UI to the boot
   * gate. Differs from handleRestart in that it does not auto-spawn — the
   * user is expected to pick "新会话 / 继续上次" again from the gate.
   */
  const handleResetMainSession = useCallback(async () => {
    if (mainPanelMounted && sessionStatus === 'running') {
      const ok = window.confirm('当前主会话将被结束，是否继续？')
      if (!ok) return
    }
    if (sessionId) {
      try {
        await window.api.cc.kill(sessionId)
      } catch {
        /* best-effort kill */
      }
    }
    setSessionId(null)
    setSessionStatus('idle')
    setMainPanelMounted(false)
    setGatePhase({ kind: 'idle' })
  }, [sessionId, sessionStatus, mainPanelMounted])

  const onPlanSelect = useCallback(
    async (value: string) => {
      // Block plan switching while session is running. The running CLI's
      // artifact path is baked at spawn time; a live switch would leave
      // the AI writing to the old target.
      if (value !== planName && sessionStatus === 'running') {
        alert('会话正在运行，请先停止（Kill）后再切换方案。')
        return
      }
      // Clear diff annotations when switching to a different plan — they
      // were gathered against the previous plan's diff context.
      if (value !== planName) {
        setDiffAnnotations([])
        setDiffGeneralNote('')
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
    // Habit signal: an external plan was imported. We record the plan name
    // (filename), not the path or contents — the design says input-side only.
    void window.api.habit.record({
      kind: 'plan_imported',
      text: `导入外部方案 ${reg.name}`,
      projectId: currentProjectId ?? undefined,
      repoPath: targetRepo || undefined,
      sourceWindow: 'main'
    })
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
      // Special case: the current plan is external (registered via import)
      // but its source file was deleted. Offer to remove the stale mapping
      // instead of the generic "let AI design" hint.
      const currentPlan = planList.find((p) => p.name === planName)
      const isDeadExternal =
        currentPlan?.source === 'external' && res.path
          ? /ENOENT|no such file|不存在|找不到|not.*exist/i.test(
              res.error ?? ''
            )
          : false
      if (isDeadExternal && currentProjectId && planName) {
        showToast(
          `外部方案文件已丢失（${res.path}）。`,
          {
            level: 'warn',
            duration: 8000,
            action: {
              label: '从列表移除',
              onClick: async () => {
                if (!projectDir) return
                const rm = await window.api.plan.removeExternal({
                  projectDir,
                  name: planName
                })
                if (!rm.ok) {
                  showToast(`移除失败：${rm.error}`, { level: 'error' })
                  return
                }
                setPlanName('')
                setPlanList((prev) => prev.filter((p) => p.name !== planName))
                knownPlanNamesRef.current.delete(planName)
                showToast('已从方案列表移除', { level: 'info' })
              }
            }
          }
        )
        return
      }
      showToast(
        `暂无可预览的方案文件${res.path ? `（${res.path}）` : ''}。请先让 AI 开始对话设计。`,
        { level: 'warn' }
      )
      return
    }
    setPlanReview({ path: res.path ?? '', content: res.content })
  }, [projectDir, planName, planList, currentProjectId])

  /** Open the git diff viewer. */
  const openDiffReview = useCallback(() => {
    if (!targetRepo) {
      showToast('本项目没有 target_repo 路径，无法打开 Diff 审查', { level: 'warn' })
      return
    }
    setDiffReviewOpen(true)
  }, [targetRepo])

  const openRepoView = useCallback(async () => {
    if (!currentProjectId || !targetRepo) {
      showToast('本项目没有 target_repo 路径，无法打开仓库查看', { level: 'warn' })
      return
    }
    const res = await window.api.repoView.openWindow(currentProjectId)
    if (!res.ok) {
      showToast(`打开仓库查看失败：${res.error ?? '未知错误'}`, { level: 'error' })
    }
  }, [currentProjectId, targetRepo])

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
        '我查看了当前方案，有以下反馈，请据此修改方案文件。',
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
        '请把以上每条意见落实到方案文件中，修改完成后在终端简述你改了什么。'
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

  /** Format diff annotations and push them into the live session.
   *  Uses cc.sendUser (chunked write + priming) not cc.write (raw): large
   *  annotation batches hitting the PTY as a single chunk get stashed by the
   *  TUI as "[Pasted Content N chars]" (bracketed-paste detection) or drop
   *  bytes, so mirror the plan-review path that already worked. */
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
      const res = await window.api.cc.sendUser(sessionId, text)
      if (!res.ok) {
        showToast(`发送批注失败：${res.error ?? '未知错误'}`, { level: 'error' })
        return
      }
      // Habit collection: record each non-empty annotation as a separate signal
      // so the aggregator can detect recurring batch-annotation patterns.
      for (const a of anns) {
        const comment = (a.comment ?? '').trim()
        if (!comment) continue
        void window.api.habit.record({
          kind: 'diff_annotation',
          text: comment,
          projectId: currentProjectId ?? undefined,
          repoPath: targetRepo || undefined,
          sourceWindow: 'diff-review'
        })
      }
      // Sent successfully — clear the batch so the next Diff 审查 starts fresh.
      setDiffAnnotations([])
      setDiffGeneralNote('')
      setDiffReviewOpen(false)
      showToast(`已发送 ${anns.length} 条批注到会话`, { level: 'info' })
    },
    [sessionId, sessionStatus, planName, getPlanAbsPath, currentProjectId, targetRepo]
  )

  const judgeExternalReviewItem = useCallback(
    async (suggestion: ExternalReviewSuggestion) => {
      if (!sessionId || sessionStatus !== 'running') {
        return { ok: false as const, error: 'session not running' }
      }
      const planAbsPath = getPlanAbsPath(planName.trim())
      return window.api.cc.judgeExternalReview({
        sessionId,
        planAbsPath,
        suggestion: {
          rawText: suggestion.rawText,
          pathHint: suggestion.pathHint,
          lineHint: suggestion.lineHint,
          linkedDiffFile: suggestion.linkedDiffFile
        }
      })
    },
    [sessionId, sessionStatus, getPlanAbsPath, planName]
  )

  const globalSearchQuickActions = [
    {
      title: '习惯监控',
      snippet: '打开习惯监控，查看托管 Chrome、活跃流程和采集设置。',
      location: '快捷入口',
      onOpen: () => setShowHabitMonitor(true)
    },
    {
      title: '托管 Chrome',
      snippet: managedChromeState.running
        ? '聚焦当前托管 Chrome 会话。'
        : '启动独立托管 Chrome 并开始网站行为采集。',
      location: '快捷入口',
      onOpen: () => {
        void handleManagedChromeAction()
      }
    },
    ...(!habitUiFlags.hideTemplatesEntry
      ? [
          {
            title: 'Prompt 模板',
            snippet: '管理和插入常用 prompt 模板。',
            location: '次级入口',
            onOpen: () => setShowTemplates(true)
          }
        ]
      : []),
    ...(!habitUiFlags.hideWizardEntry
      ? [
          {
            title: '新手向导',
            snippet: '重新打开新手上手向导。',
            location: '次级入口',
            onOpen: () => setShowOnboarding(true)
          }
        ]
      : [])
  ]

  const commandPaletteCommands: Command[] = [
    {
      id: 'proj.picker',
      label: '📁 项目管理（切换 / 新建 / 删除）',
      keywords: 'project switch new',
      action: () => setShowProjectPicker(true)
    },
    {
      id: 'doctor',
      label: '🩺 CLI 体检',
      keywords: 'doctor check health',
      action: () => setShowDoctor(true)
    },
    {
      id: 'settings',
      label: '⚙️ 设置',
      keywords: 'settings command ai cli',
      action: () => setShowAiSettings(true)
    },
    {
      id: 'habit-monitor',
      label: '🧠 习惯监控',
      keywords: 'habit monitor flows automation',
      action: () => setShowHabitMonitor(true)
    },
    {
      id: 'managed-chrome',
      label: managedChromeState.running
        ? '🌐 聚焦托管 Chrome'
        : '🌐 启动托管 Chrome',
      keywords: 'managed chrome browser',
      action: () => {
        void handleManagedChromeAction()
      }
    },
    ...(!habitUiFlags.hideTemplatesEntry
      ? [
          {
            id: 'tpl',
            label: '📋 Prompt 模板',
            keywords: 'templates prompt snippets',
            action: () => setShowTemplates(true)
          }
        ]
      : []),
    ...(!habitUiFlags.hideWizardEntry
      ? [
          {
            id: 'onboard',
            label: '❓ 新手向导',
            keywords: 'help onboarding wizard',
            action: () => setShowOnboarding(true)
          }
        ]
      : []),
    {
      id: 'search',
      label: '🔍 全局搜索',
      hint: 'Ctrl+Shift+F',
      keywords: 'find search',
      action: () => setShowGlobalSearch(true),
      disabled: !hasProject
    },
    {
      id: 'build-panel',
      label: '🏗️ 项目构建',
      keywords: 'build compile msys visual studio',
      action: () => setShowBuildPanel(true),
      disabled: !hasProject
    },
    {
      id: 'logs',
      label: '📣 错误与通知',
      keywords: 'errors log notifications',
      action: () => setShowErrors(true)
    },
    {
      id: 'toggle-theme',
      label: theme === 'dark' ? '切换到浅色主题' : '切换到暗色主题',
      keywords: 'theme dark light color',
      action: handleToggleTheme
    },
    {
      id: 'plan-review',
      label: '📝 审阅当前方案',
      keywords: 'plan review annotate',
      action: () => void openPlanReview(),
      disabled: !hasProject || !planName.trim()
    },
    {
      id: 'diff-review',
      label: '🔀 Diff 审查',
      keywords: 'diff review code',
      action: () => void openDiffReview(),
      disabled: !hasProject
    }
  ]


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
          title="配置全局截图快捷键，以及 AI CLI 命令 / 参数 / 环境变量"
        >
          ⚙️ 设置
        </button>
        <button
          className="topbar-btn"
          onClick={() => setShowHabitMonitor(true)}
          title="查看习惯采集、托管 Chrome 和自动化流程"
        >
          🧠 习惯监控
        </button>
        <button
          className="topbar-btn"
          onClick={() => void handleManagedChromeAction()}
          disabled={managedChromeBusy}
          title={
            managedChromeState.running
              ? '聚焦当前托管 Chrome 会话'
              : '启动托管 Chrome 并开始网站行为采集'
          }
        >
          🌐 托管 Chrome
        </button>
        {mainPanelMounted && (
          <button
            className="topbar-btn"
            onClick={() => void handleResetMainSession()}
            title="结束当前主会话，回到选择界面"
          >
            🔄 重置主会话
          </button>
        )}
        <button
          className={`topbar-btn ${errorCount > 0 ? 'topbar-btn-danger' : ''}`}
          onClick={() => setShowErrors((s) => !s)}
          title="查看错误与通知日志"
        >
          {errorCount > 0 ? `⚠ ${errorCount}` : '📣 日志'}
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

      {hasProject && (
        <div className="plan-name-bar">
          <span className="plan-progress-label">📋 方案：</span>
          <select
            className="plan-name-input"
            value={
              planName && planList.some((p) => p.name === planName)
                ? planName
                : '__NEW__'
            }
            onChange={(e) => void onPlanSelect(e.target.value)}
            disabled={sessionStatus === 'running'}
            title={
              sessionStatus === 'running'
                ? '运行中无法切换方案，请先停止'
                : '选择已有方案或新建'
            }
          >
            <option value="__NEW__">+ 新建方案</option>
            {planList.map((p) => (
              <option key={p.name} value={p.name}>
                {p.source === 'external' ? '📥 ' : ''}
                {p.name}
              </option>
            ))}
          </select>
          {!planList.some((p) => p.name === planName) && (
            <input
              type="text"
              className="plan-name-input"
              placeholder="输入新方案名（例如 add-auth）"
              value={planName}
              onChange={(e) => setPlanName(e.target.value)}
              disabled={sessionStatus === 'running'}
              style={{ flex: 1 }}
            />
          )}
          <button
            className="topbar-btn"
            onClick={() => void onImportExternal()}
            disabled={sessionStatus === 'running' || !currentProjectId}
            title="导入外部方案 md 文件（会归档到外部原路径）"
          >
            📥 导入外部方案
          </button>
          <button
            className="topbar-btn"
            onClick={() => setShowBuildPanel(true)}
            disabled={!currentProjectId}
            title="打开项目构建面板"
          >
            构建
          </button>
          {planName.trim() && (
            <button
              className="topbar-btn"
              onClick={() => void openPlanReview()}
              disabled={!currentProjectId}
              title="查看 / 编辑当前方案的 md"
            >
              👁 方案预览
            </button>
          )}
          <span
            className={`plan-progress-node ${sessionStatus === 'running' ? 'running' : sessionStatus === 'exited' ? 'done' : ''}`}
            title={`会话状态：${sessionStatus}`}
            style={{ marginLeft: 'auto' }}
          >
            {sessionStatus === 'running'
              ? '⏳ 运行中'
              : sessionStatus === 'exited'
                ? '✅ 已完成'
                : '─ 未启动'}
          </span>
        </div>
      )}

      <div className="main-split">
        {mainPanelMounted ? (
          <MainPanel
            sessionId={sessionId ?? ''}
            projectId={currentProjectId ?? ''}
            projectDir={projectDir}
            cwd={targetRepo}
            planName={planName}
            status={sessionStatus}
            onStart={() => void handleStart('new')}
            onStop={handleStop}
            onRestart={handleRestart}
            onOpenRepoView={() => void openRepoView()}
            onOpenDiff={() => setDiffReviewOpen(true)}
            disabled={!currentProjectId || !planName.trim()}
            repoViewDisabled={!currentProjectId}
          />
        ) : (
          <MainBootGate
            phase={gatePhase}
            command={aiSettings.command ?? aiSettings.ai_cli ?? 'claude'}
            planName={planName}
            disabled={!currentProjectId || !planName.trim()}
            onChoose={(mode) => void handleStart(mode)}
            onDismissFailure={() => setGatePhase({ kind: 'idle' })}
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
          quickActions={globalSearchQuickActions}
          onClose={() => setShowGlobalSearch(false)}
        />
      )}
      {showCmdk && (
        <CommandPalette
          onClose={() => setShowCmdk(false)}
          commands={commandPaletteCommands}
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
          aiCli={aiSettings.ai_cli}
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
          annotations={diffAnnotations}
          onAnnotationsChange={setDiffAnnotations}
          generalNote={diffGeneralNote}
          onGeneralNoteChange={setDiffGeneralNote}
          mode={diffMode}
          onModeChange={setDiffMode}
          selectedCommit={diffSelectedCommit}
          onSelectedCommitChange={setDiffSelectedCommit}
          selectedFile={diffSelectedFile}
          onSelectedFileChange={setDiffSelectedFile}
          onJudgeExternalReviewItem={judgeExternalReviewItem}
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
      {showTemplates && (
        <TemplatesDialog
          sessionId={sessionId}
          sessionRunning={sessionStatus === 'running'}
          onClose={() => setShowTemplates(false)}
          onInject={(sid, text) => {
            void window.api.cc.sendUser(sid, text)
            // Habit signal: a template was used. We record the template body
            // so it clusters together with the corresponding ai_prompt_main
            // events naturally — same shingles will match.
            void window.api.habit.record({
              kind: 'template_used',
              text,
              projectId: currentProjectId ?? undefined,
              repoPath: targetRepo || undefined,
              sourceWindow: 'main'
            })
          }}
        />
      )}
      {showHabitMonitor && (
        <HabitMonitorDialog
          onClose={() => {
            setShowHabitMonitor(false)
            void refreshHabitMonitorSnapshot()
          }}
          onOpenAiSettings={() => {
            setShowHabitMonitor(false)
            setShowAiSettings(true)
          }}
          mainCliLabel={getCliTargetLabel(aiSettings.ai_cli)}
        />
      )}
      {showHabitFirstRun && (
        <FirstRunNoticeDialog
          onAcknowledge={() => {
            void window.api.habit.settings.update({
              firstRunNoticeShownAt: Date.now()
            })
            void refreshHabitMonitorSnapshot()
            setShowHabitFirstRun(false)
          }}
          onDisableCollection={() => {
            void window.api.habit.settings.update({
              enabled: false,
              firstRunNoticeShownAt: Date.now()
            })
            setHabitSettingsSnapshot((current) =>
              current
                ? {
                    ...current,
                    enabled: false,
                    firstRunNoticeShownAt: Date.now()
                  }
                : current
            )
            setShowHabitFirstRun(false)
          }}
        />
      )}
      {showAiSettings && (
        <AiSettingsDialog
          projectId={currentProjectId}
          initial={aiSettings}
          initialRepoView={repoViewAiSettings}
          initialAppSettings={appSettings}
          initialBuildConfig={visibleProjectBuildConfig}
          buildConfigReady={projectBuildConfigReady}
          visualStudioInstallations={visualStudioInstallations}
          visualStudioInstallationsLoading={visualStudioInstallationsLoading}
          onRefreshVisualStudioInstallations={() => {
            void refreshVisualStudioInstallations()
          }}
          onClose={() => setShowAiSettings(false)}
          onSaved={(next) => {
            // If the main-session CLI binary changes while a session is
            // mounted, the existing PTY is running the previous CLI — its
            // saved conversation is not addressable from the new CLI.
            // Reset the session to keep "继续上次" honest.
            const prevCli = aiSettings.command ?? aiSettings.ai_cli
            const nextCli = next.command ?? next.ai_cli
            setAiSettings(next)
            if (prevCli !== nextCli && mainPanelMounted) {
              void handleResetMainSession()
            }
          }}
          onSavedRepoView={(next) => setRepoViewAiSettings(next)}
          onSavedAppSettings={(next) => setAppSettings(next)}
          onSavedBuildConfig={(next) => {
            setProjectBuildConfig(next)
            setProjectBuildConfigProjectId(currentProjectId)
          }}
        />
      )}
      {showBuildPanel && (
        <ProjectBuildPanel
          open={showBuildPanel}
          currentProjectId={currentProjectId}
          currentProjectName={projectName || null}
          buildConfig={visibleProjectBuildConfig}
          buildConfigReady={projectBuildConfigReady}
          state={buildStateForCurrentProject}
          sessionId={sessionId}
          sessionStatus={sessionStatus}
          onClose={() => setShowBuildPanel(false)}
          onStartBuild={() => void handleStartBuild()}
          onStopBuild={() => void handleStopBuild()}
          onAnalyzeFailure={() => void handleAnalyzeBuildFailure()}
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
