import { useCallback, useEffect, useRef, useState } from 'react'
import { FileDiffIcon } from '@primer/octicons-react'
import { getTheme, toggleTheme } from './utils/theme.js'
import {
  formatAnnotationsForSession,
  planNameToFilename
} from './utils/session-message-format'
import { buildCliLaunchArgs } from './utils/cliLaunchArgs'
import { buildRuntimeLogAnalysisMessage } from './utils/runtimeLogAnalysisMessage'
import {
  nextRuntimeLogCommentAfterSendResult,
  nextRuntimeLogDialogOpenAfterSendResult
} from './utils/runtimeLogDialogState'
import { selectVisibleRuntimeState } from './utils/runtimeStateSelection'
import {
  canStartMainSession,
  formatMainSessionPlanLabel
} from './utils/mainSessionPlanMode'
import MainPanel from './components/MainPanel'
import MainBootGate, { type BootGatePhase } from './components/MainBootGate'
import ProjectPicker, { type ProjectInfo } from './components/ProjectPicker'
import ErrorPanel, { pushLog, useLogs } from './components/ErrorPanel'
import AiSettingsDialog, {
  DEFAULT_AI_CLI,
  type AiSettings,
  type AppSettings,
  type SettingsSectionKey
} from './components/AiSettingsDialog'
import ProjectBuildPanel, {
  getBuildStartBlockedReason,
  getRuntimeStartBlockedReason,
} from './components/ProjectBuildPanel'
import RuntimeLogDialog from './components/RuntimeLogDialog'
import TemplatesDialog from './components/TemplatesDialog'
import ScheduledTaskDialog from './scheduled-tasks/ScheduledTaskDialog'
import NormalTaskDialog, {
  type NormalTaskEntry,
  type NormalTaskMetadataDraft
} from './normal-tasks/NormalTaskDialog'
import { buildNormalTaskRunPrompt } from './normal-tasks/normalTaskPrompt'
import RemoteImDrawer from './remote-im/RemoteImDrawer'
import RemoteImSummaryDialog from './remote-im/RemoteImSummaryDialog'
import { mergeRemoteImMessages } from './remote-im/messageMerge'
import RemoteImClientHost from './remote-im/RemoteImClientHost'
import RemoteImLoginDialog, {
  type RemoteImLoginSubmitInput
} from './remote-im/RemoteImLoginDialog'
import {
  addRemoteImContact,
  getRemoteImConversations,
  getRemoteImStatusLabel
} from './remote-im/remoteImViewModel'
import {
  isRemoteImAccountReady,
  shouldPromptRemoteImStartupLogin
} from './remote-im/remoteImLoginFlow'
import {
  forgetRemoteImOutgoingImageFile,
  registerRemoteImOutgoingImageFile
} from './remote-im/outgoingImageRegistry'
import OnboardingWizard from './components/OnboardingWizard'
import CommandPalette, { type Command } from './components/CommandPalette'
import ToastHost, { showToast } from './components/Toast'
import GlobalSearchDialog from './components/GlobalSearchDialog'
import PlanReviewDialog, { type Annotation } from './components/PlanReviewDialog'
import DiffViewerDialog, { type DiffAnnotation } from './components/DiffViewerDialog'
import type { DiffMode } from './components/diffViewerConfig'
import type { ExternalReviewSuggestion } from './components/externalAiReview'
import type {
  BuildRuntimeState,
  ProjectBuildConfig,
  ProjectRuntimeConfig,
  RuntimeState,
  RemoteImAccountConfig,
  RemoteImConfig,
  RemoteImContactRelation,
  RemoteImLoginState,
  RemoteImMessage,
  RemoteImStatus,
  VisualStudioInstallation
} from '../electron/preload'

const LAST_PROJECT_KEY = 'multi-ai-code.lastProjectId'
const DEFAULT_PROJECT_BUILD_CONFIG: ProjectBuildConfig = { enabled: false, steps: [] }
const DEFAULT_PROJECT_RUNTIME_CONFIG: ProjectRuntimeConfig = {
  enabled: false,
  cwd: '.',
  command: '',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}
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
const DEFAULT_RUNTIME_STATE: RuntimeState = {
  status: 'idle',
  projectId: null,
  projectName: null,
  targetRepo: null,
  cwd: null,
  command: null,
  envType: null,
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: null,
  startedAt: null,
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: ''
}
const DEFAULT_REMOTE_IM_CONFIG: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: null,
  desktopUserId: '',
  desktopRole: 'master',
  userSigMode: 'endpoint',
  userSigEndpoint: '',
  userSigSecretKey: '',
  friendUserIds: [],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: [],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 4000
}

type WorkMode = 'task-watch' | 'plan-design'

function appendBuildLog(current: string, chunk: string): string {
  const next = current + chunk
  if (next.length <= BUILD_LOG_LIMIT) return next
  return `...[build log truncated]...\n${next.slice(-BUILD_LOG_LIMIT)}`
}

function appendRuntimeLog(current: string, chunk: string): string {
  const next = current + chunk
  if (next.length <= BUILD_LOG_LIMIT) return next
  return `...[runtime log truncated]...\n${next.slice(-BUILD_LOG_LIMIT)}`
}

function verifyNormalTaskMetadataSaved(
  items: NormalTaskEntry[],
  name: string,
  metadata: NormalTaskMetadataDraft
): boolean {
  const saved = items.find((item) => item.name === name)
  if (!saved) return false
  return (
    (saved.description ?? '').trim() === metadata.description.trim() &&
    (saved.details ?? '').trim() === metadata.details.trim()
  )
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

  return <AccountGate />
}

/**
 * 登录门（首屏）：账号绑定成功前只渲染登录表单，绑定后才挂载 AppShell。
 *
 * 关键——把 AppShell 做成“绑定后才挂载的子组件”，而不是仅用视图分支遮挡：AppShell 里
 * 加载项目/触库的 useEffect 只有在它被挂载时才运行，从而保证登录前不会用错误的（未按
 * 账号作用域解析的）rootDir 打开数据库。账号 = 用户输入的 desktopUserId，数据按账号隔离
 * 到 <base>/accounts/<账号>/，同账号第二个窗口会被单实例锁挡回。
 */
function AccountGate(): JSX.Element {
  const [bound, setBound] = useState(false)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const handleSubmit = async (input: RemoteImLoginSubmitInput): Promise<void> => {
    setSaving(true)
    setError(null)
    try {
      const res = await window.api.remoteIm.bindAccount(input.account)
      if (res.ok) {
        setBound(true)
      } else {
        setError(
          res.alreadyLocked
            ? '该账号已在另一个 Multi-AI Code 窗口打开，请切换到那个窗口。'
            : res.error || '登录失败，请重试。'
        )
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err))
    } finally {
      setSaving(false)
    }
  }

  if (bound) return <AppShell />

  return (
    <RemoteImLoginDialog
      open
      variant="gate"
      loginState={null}
      projectConfig={null}
      projectConfigReady={false}
      saving={saving}
      error={error}
      onClose={() => {
        /* 登录门不可关闭：账号是进入应用的前置条件 */
      }}
      onSubmit={handleSubmit}
    />
  )
}

function AppShell() {
  const [projects, setProjects] = useState<ProjectInfo[]>([])
  const [currentProjectId, setCurrentProjectId] = useState<string | null>(null)
  const [showProjectPicker, setShowProjectPicker] = useState(false)
  const [planName, setPlanName] = useState('')
  const [workMode, setWorkMode] = useState<WorkMode>('plan-design')
  const isTaskWatchMode = workMode === 'task-watch'
  const isPlanDesignMode = workMode === 'plan-design'
  const noPlanMode = isTaskWatchMode
  const [showErrors, setShowErrors] = useState(false)
  const [showAiSettings, setShowAiSettings] = useState(false)
  const [aiSettingsInitialSection, setAiSettingsInitialSection] =
    useState<SettingsSectionKey>('shortcut')
  const [aiSettings, setAiSettings] = useState<AiSettings>({ ai_cli: DEFAULT_AI_CLI })
  const [repoViewAiSettings, setRepoViewAiSettings] = useState<AiSettings>({
    ai_cli: DEFAULT_AI_CLI
  })
  const [aiSettingsReady, setAiSettingsReady] = useState(false)
  const [aiSettingsLoadError, setAiSettingsLoadError] = useState<string | null>(null)
  const [appSettings, setAppSettings] = useState<AppSettings>({
    screenshotShortcutEnabled: true,
    screenshotShortcut: 'CommandOrControl+Shift+A',
    showDevToolbarButtons: false
  })
  const [projectBuildConfig, setProjectBuildConfig] = useState<ProjectBuildConfig>(
    DEFAULT_PROJECT_BUILD_CONFIG
  )
  const [projectBuildConfigProjectId, setProjectBuildConfigProjectId] = useState<string | null>(
    null
  )
  const [projectRuntimeConfig, setProjectRuntimeConfig] = useState<ProjectRuntimeConfig>(
    DEFAULT_PROJECT_RUNTIME_CONFIG
  )
  const [projectRuntimeConfigProjectId, setProjectRuntimeConfigProjectId] = useState<
    string | null
  >(null)
  const [remoteImConfig, setRemoteImConfig] =
    useState<RemoteImConfig>(DEFAULT_REMOTE_IM_CONFIG)
  const [remoteImConfigProjectId, setRemoteImConfigProjectId] = useState<string | null>(null)
  const [remoteImStatus, setRemoteImStatus] = useState<RemoteImStatus | null>(null)
  const [remoteImMessages, setRemoteImMessages] = useState<RemoteImMessage[]>([])
  // 各会话「向上翻页」是否已翻到最早（true=没有更早了，隐藏加载更早按钮）。
  const [remoteImEarlierExhausted, setRemoteImEarlierExhausted] = useState<Record<string, boolean>>({})
  const [remoteImSelectedPeerUserId, setRemoteImSelectedPeerUserId] = useState<string | null>(null)
  const [remoteImInput, setRemoteImInput] = useState('')
  const [remoteImLoginState, setRemoteImLoginState] = useState<RemoteImLoginState | null>(null)
  const [remoteImLoginRequested, setRemoteImLoginRequested] = useState(false)
  const [showRemoteImLogin, setShowRemoteImLogin] = useState(false)
  const [remoteImLoginSaving, setRemoteImLoginSaving] = useState(false)
  const [remoteImLoginError, setRemoteImLoginError] = useState<string | null>(null)
  const [visualStudioInstallations, setVisualStudioInstallations] = useState<
    VisualStudioInstallation[]
  >([])
  const [visualStudioInstallationsLoading, setVisualStudioInstallationsLoading] =
    useState(false)
  const [showTemplates, setShowTemplates] = useState(false)
  const [showNormalTaskDialog, setShowNormalTaskDialog] = useState(false)
  const [showScheduledTaskDialog, setShowScheduledTaskDialog] = useState(false)
  const [showRemoteImDrawer, setShowRemoteImDrawer] = useState(false)
  const [showRemoteImSummary, setShowRemoteImSummary] = useState(false)
  const [showOnboarding, setShowOnboarding] = useState(false)
  const [showCmdk, setShowCmdk] = useState(false)
  const [showGlobalSearch, setShowGlobalSearch] = useState(false)
  const [showBuildPanel, setShowBuildPanel] = useState(false)
  const [showRuntimeLogDialog, setShowRuntimeLogDialog] = useState(false)
  const [runtimeLogComment, setRuntimeLogComment] = useState('')
  const [theme, setThemeState] = useState<'light' | 'dark'>(() => getTheme())

  const visibleProjectBuildConfig =
    currentProjectId !== null && projectBuildConfigProjectId === currentProjectId
      ? projectBuildConfig
      : DEFAULT_PROJECT_BUILD_CONFIG
  const projectBuildConfigReady =
    currentProjectId !== null && projectBuildConfigProjectId === currentProjectId
  const visibleProjectRuntimeConfig =
    currentProjectId !== null && projectRuntimeConfigProjectId === currentProjectId
      ? projectRuntimeConfig
      : DEFAULT_PROJECT_RUNTIME_CONFIG
  const projectRuntimeConfigReady =
    currentProjectId !== null && projectRuntimeConfigProjectId === currentProjectId
  const remoteImConfigReady =
    currentProjectId !== null && remoteImConfigProjectId === currentProjectId

  // Single-stage session state
  const [sessionId, setSessionId] = useState<string | null>(null)
  const [sessionStatus, setSessionStatus] = useState<'idle' | 'running' | 'exited'>('idle')
  // MainPanel is mounted (true) or the boot gate is shown (false). Toggled
  // by spawn success (→ true) and by CLI/config reset paths or resume-failure (→ false).
  // sessionStatus === 'exited' deliberately keeps mainPanelMounted=true so
  // the previous terminal scrollback and the existing 重启 button still work.
  const [mainPanelMounted, setMainPanelMounted] = useState(false)
  // Gate's internal display state when mainPanelMounted === false.
  const [gatePhase, setGatePhase] = useState<BootGatePhase>({ kind: 'idle' })

  const handleToggleTheme = useCallback(() => {
    const next = toggleTheme()
    setThemeState(next)
    // 让运行中的 AICLI TUI（codex / opencode）跟随明暗，无需重启会话。
    window.api.cc.setTerminalTheme(next)
  }, [])

  const [stageConfigs, setStageConfigs] = useState<
    Record<string, { command?: string; args?: string[]; env?: Record<string, string>; skip?: boolean }>
  >({})
  const [planList, setPlanList] = useState<NormalTaskEntry[]>([])
  const [msysEnabled, setMsysEnabled] = useState(false)
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
  const [runtimeState, setRuntimeState] = useState<RuntimeState>(DEFAULT_RUNTIME_STATE)
  const [logs] = useLogs()
  const errorCount = logs.filter((l) => l.level === 'error' || l.level === 'warn').length

  const openAiSettingsSection = useCallback((section: SettingsSectionKey = 'shortcut') => {
    setAiSettingsInitialSection(section)
    setShowAiSettings(true)
  }, [])

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
    let cancelled = false
    void window.api.runtime.getState().then((state) => {
      if (cancelled) return
      setRuntimeState(state)
    })
    const offStatus = window.api.runtime.onStatus((state) => {
      if (cancelled) return
      setRuntimeState(state)
    })
    const offData = window.api.runtime.onData((event) => {
      if (cancelled || !event.chunk) return
      setRuntimeState((prev) => ({
        ...prev,
        projectId: event.projectId ?? prev.projectId,
        log: appendRuntimeLog(prev.log, event.chunk)
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
    // 版本号展示在原生窗口标题栏，主界面 meta 里不再重复。
    window.api.version().then((v) => {
      document.title = v ? `Multi-AI Code v${v}` : 'Multi-AI Code'
    })
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
  const runtimeStateForCurrentProject =
    currentProjectId !== null && runtimeState.projectId === currentProjectId
      ? runtimeState
      : DEFAULT_RUNTIME_STATE
  const visibleRuntimeState = selectVisibleRuntimeState(
    currentProjectId,
    runtimeState,
    DEFAULT_RUNTIME_STATE
  )
  const hasProject = currentProject !== null
  const canStartCurrentMainSession = canStartMainSession(
    currentProjectId,
    noPlanMode,
    planName
  )
  const mainSessionPlanLabel = formatMainSessionPlanLabel(noPlanMode, planName)
  const mainSessionStatusLabel =
    sessionStatus === 'running' ? '运行中' : sessionStatus === 'exited' ? '已退出' : '待启动'
  const runtimeStartBlockedReason = getRuntimeStartBlockedReason(
    currentProjectId,
    projectRuntimeConfigReady,
    visibleProjectRuntimeConfig,
    visibleRuntimeState
  )
  const runtimeTopbarRunning = visibleRuntimeState.status === 'running'
  const runtimeTopbarDisabled = !runtimeTopbarRunning && runtimeStartBlockedReason !== null

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
  const applyPlanList = useCallback(
    (items: NormalTaskEntry[]) => {
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
        return prev
      })
      // Update "ever seen" set AFTER the prune check.
      for (const n of currentNames) knownPlanNamesRef.current.add(n)
    },
    []
  )

  const refreshPlanList = useCallback(async () => {
    if (!currentProjectId || !projectDir) {
      setPlanList([])
      knownPlanNamesRef.current = new Set()
      return []
    }
    const planRes = await window.api.plan.list(projectDir)
    if (!planRes.ok) {
      showToast(planRes.error ?? '读取普通任务列表失败', { level: 'error' })
      return planList
    }
    applyPlanList(planRes.items)
    return planRes.items
  }, [applyPlanList, currentProjectId, projectDir, planList])

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
      applyPlanList(planRes.items)
    })()
    return () => {
      cancelled = true
    }
  }, [applyPlanList, currentProjectId, projectDir])

  useEffect(() => {
    if (!currentProjectId) {
      setStageConfigs({})
      setMsysEnabled(false)
      setAiSettings({ ai_cli: DEFAULT_AI_CLI })
      setRepoViewAiSettings({ ai_cli: DEFAULT_AI_CLI })
      setProjectBuildConfig(DEFAULT_PROJECT_BUILD_CONFIG)
      setProjectBuildConfigProjectId(null)
      setProjectRuntimeConfig(DEFAULT_PROJECT_RUNTIME_CONFIG)
      setProjectRuntimeConfigProjectId(null)
      setRemoteImConfig(DEFAULT_REMOTE_IM_CONFIG)
      setRemoteImConfigProjectId(null)
      setRemoteImStatus(null)
      setRemoteImMessages([])
      setRemoteImSelectedPeerUserId(null)
      setAiSettingsReady(false)
      setAiSettingsLoadError(null)
      return
    }
    localStorage.setItem(LAST_PROJECT_KEY, currentProjectId)
    void window.api.project.touch(currentProjectId)
    let cancelled = false
    setProjectBuildConfig(DEFAULT_PROJECT_BUILD_CONFIG)
    setProjectBuildConfigProjectId(null)
    setProjectRuntimeConfig(DEFAULT_PROJECT_RUNTIME_CONFIG)
    setProjectRuntimeConfigProjectId(null)
    setRemoteImConfig(DEFAULT_REMOTE_IM_CONFIG)
    setRemoteImConfigProjectId(null)
    setRemoteImStatus(null)
    setRemoteImMessages([])
    setRemoteImSelectedPeerUserId(null)
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

      const runtimeResult = await window.api.project.getRuntimeConfig(currentProjectId)
      if (cancelled) return
      if (!runtimeResult.ok) {
        setProjectRuntimeConfig(DEFAULT_PROJECT_RUNTIME_CONFIG)
        setProjectRuntimeConfigProjectId(currentProjectId)
        showToast(runtimeResult.error ?? '读取项目运行配置失败', { level: 'error' })
      } else {
        setProjectRuntimeConfig(runtimeResult.value ?? DEFAULT_PROJECT_RUNTIME_CONFIG)
        setProjectRuntimeConfigProjectId(currentProjectId)
      }
      const runtimeConfigRepaired = runtimeResult.ok && runtimeResult.repaired === true

      const remoteImResult = await window.api.remoteIm.getConfig(currentProjectId)
      if (cancelled) return
      if (!remoteImResult.ok) {
        setRemoteImConfig(DEFAULT_REMOTE_IM_CONFIG)
        setRemoteImConfigProjectId(currentProjectId)
        showToast(remoteImResult.error ?? '读取远程 IM 配置失败', { level: 'error' })
      } else {
        setRemoteImConfig(remoteImResult.value ?? DEFAULT_REMOTE_IM_CONFIG)
        setRemoteImConfigProjectId(currentProjectId)
      }
      const status = await window.api.remoteIm.getStatus(currentProjectId)
      if (!cancelled) setRemoteImStatus(status)
      const messages = await window.api.remoteIm.listMessages(currentProjectId, 500)
      if (!cancelled) setRemoteImMessages(messages)

      const aiResult = await window.api.project.getAiSettings(currentProjectId)
      if (cancelled) return
      if (!aiResult.ok) {
        setAiSettingsReady(false)
        setAiSettingsLoadError(aiResult.error ?? '读取主会话 AI 设置失败')
        showToast(aiResult.error ?? '读取主会话 AI 设置失败', { level: 'error' })
        return
      }
      setAiSettings(aiResult.value ?? { ai_cli: DEFAULT_AI_CLI })
      setAiSettingsReady(true)
      setAiSettingsLoadError(null)

      const repoResult = await window.api.project.getRepoViewAiSettings(currentProjectId)
      if (cancelled) return
      if (!repoResult.ok) {
        showToast(repoResult.error ?? '读取仓库查看 AI 设置失败', { level: 'error' })
        return
      }
      setRepoViewAiSettings(repoResult.value ?? { ai_cli: DEFAULT_AI_CLI })

      if (aiResult.repaired || repoResult.repaired || buildConfigRepaired || runtimeConfigRepaired) {
        showToast('项目设置文件已自动修复', { level: 'success' })
      }
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId])

  useEffect(() => {
    let cancelled = false
    void window.api.remoteIm.getLoginState().then((result) => {
      if (cancelled) return
      if (result.ok) {
        setRemoteImLoginState(result.value)
        setRemoteImLoginRequested(isRemoteImAccountReady(result.value?.account))
        if (shouldPromptRemoteImStartupLogin(result.value)) {
          setShowRemoteImLogin(true)
        }
      } else {
        setRemoteImLoginError(result.error ?? '读取远程 IM 登录状态失败')
      }
    })
    return () => {
      cancelled = true
    }
  }, [])

  useEffect(() => {
    const offStatus = window.api.remoteIm.onStatus((status) => {
      if (status.projectId === currentProjectId) setRemoteImStatus(status)
    })
    const offMessages = window.api.remoteIm.onMessagesChanged((evt) => {
      if (!currentProjectId || evt.projectId !== currentProjectId) return
      void window.api.remoteIm
        .listMessages(currentProjectId, 500)
        .then((messages) => setRemoteImMessages(messages))
    })
    return () => {
      offStatus()
      offMessages()
    }
  }, [currentProjectId])

  useEffect(() => {
    if (!remoteImConfigReady) return
    const conversations = getRemoteImConversations(remoteImConfig, remoteImMessages)
    if (
      remoteImSelectedPeerUserId &&
      conversations.some((conversation) => conversation.userId === remoteImSelectedPeerUserId)
    ) {
      return
    }
    setRemoteImSelectedPeerUserId(conversations[0]?.userId ?? null)
  }, [remoteImConfigReady, remoteImConfig, remoteImMessages, remoteImSelectedPeerUserId])

  useEffect(() => {
    if (!isTaskWatchMode || !currentProjectId) return
    void window.api.scheduledTasks.scanNow(currentProjectId)
  }, [isTaskWatchMode, currentProjectId])

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
    if (!currentProjectId || !canStartMainSession(currentProjectId, noPlanMode, planName)) return
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
    const command = aiSettings.command ?? aiSettings.ai_cli ?? DEFAULT_AI_CLI
    const args = buildCliLaunchArgs(
      aiSettings.ai_cli ?? DEFAULT_AI_CLI,
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
      planName: '',
      planMode: 'none',
      planAbsPath: undefined,
      planPending: false,
      allowScheduledTasks: isTaskWatchMode,
      initialUserMessage: undefined,
      command,
      args,
      env: aiSettings.env ?? {},
      opencode: aiSettings.opencode,
      terminalTheme: theme,
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
    if (isTaskWatchMode) {
      void window.api.scheduledTasks.scanNow(currentProjectId)
    }
  }, [currentProjectId, noPlanMode, isTaskWatchMode, planName, projects, aiSettings, aiSettingsReady, aiSettingsLoadError])

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

  const handleStartBuild = useCallback(async (
    scope: 'all' | 'single-step',
    stepId: string | null = null
  ) => {
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
      visibleProjectBuildConfig,
      scope,
      stepId
    )
    if (blockedReason) {
      showToast(blockedReason, { level: 'warn' })
      setShowBuildPanel(true)
      return
    }
    if (!currentProjectId) return

    const result = await window.api.build.start(currentProjectId, { scope, stepId })
    setBuildState(result.state)
    setShowBuildPanel(true)
    if (!result.ok) {
      showToast(result.error ?? '启动构建失败', { level: 'error' })
    }
  }, [buildState.projectId, buildState.status, currentProjectId, projectBuildConfigReady, visibleProjectBuildConfig])

  const handleStopBuild = useCallback(async () => {
    const result = await window.api.build.stop()
    if (!result.ok) {
      showToast(result.error ?? '停止构建失败', { level: 'error' })
    }
  }, [])

  const handleStartRuntime = useCallback(async () => {
    if (
      runtimeState.projectId !== null &&
      runtimeState.projectId !== currentProjectId &&
      runtimeState.status === 'running'
    ) {
      showToast('另一个项目的运行进程仍在运行，请先停止后再启动当前项目运行', {
        level: 'warn'
      })
      setShowRuntimeLogDialog(true)
      return
    }
    if (!currentProjectId) return

    setShowRuntimeLogDialog(true)
    const result = await window.api.runtime.start(currentProjectId)
    setRuntimeState(result.state)
    if (!result.ok) {
      showToast(result.error ?? '启动运行失败', { level: 'error' })
    }
  }, [currentProjectId, runtimeState.projectId, runtimeState.status])

  const handleStopRuntime = useCallback(async () => {
    const result = await window.api.runtime.stop()
    if (!result.ok) {
      showToast(result.error ?? '停止运行失败', { level: 'error' })
    }
  }, [])

  const handleSendRemoteImLocalMessage = useCallback(async (toUserId?: string | null) => {
    if (!currentProjectId) return
    const text = remoteImInput.trim()
    if (!text) return
    const peerUserId = toUserId?.trim() || remoteImSelectedPeerUserId?.trim() || ''
    if (!peerUserId) {
      showToast('请选择要发送的联系人', { level: 'warn' })
      return
    }
    const result = await window.api.remoteIm.sendPeerMessage(currentProjectId, text, peerUserId)
    if (!result.ok) {
      showToast(result.error ?? '发送远程 IM 消息失败', { level: 'error' })
      return
    }
    setRemoteImSelectedPeerUserId(result.toUserId ?? peerUserId)
    setRemoteImInput('')
    const messages = await window.api.remoteIm.listMessages(currentProjectId, 500)
    setRemoteImMessages(messages)
  }, [currentProjectId, remoteImInput, remoteImSelectedPeerUserId])

  const handleSendRemoteImImage = useCallback(async (toUserId: string, file: File) => {
    if (!currentProjectId) return
    const peerUserId = toUserId.trim() || remoteImSelectedPeerUserId?.trim() || ''
    if (!peerUserId) {
      showToast('请选择要发送的联系人', { level: 'warn' })
      return
    }
    if (!file.type.startsWith('image/')) {
      showToast('请选择图片文件', { level: 'warn' })
      return
    }
    const maxImageBytes = 20 * 1024 * 1024
    if (file.size > maxImageBytes) {
      showToast('图片不能超过 20 MB', { level: 'warn' })
      return
    }

    const fileToken = registerRemoteImOutgoingImageFile(file)
    const localPath = window.api.getPathForFile(file) || null
    const result = await window.api.remoteIm.sendPeerImage(currentProjectId, {
      fileToken,
      toUserId: peerUserId,
      localPath,
      fileName: file.name || null,
      mimeType: file.type || null,
      sizeBytes: file.size
    })
    if (!result.ok) {
      forgetRemoteImOutgoingImageFile(fileToken)
      showToast(result.error ?? '发送远程 IM 图片失败', { level: 'error' })
      return
    }
    setRemoteImSelectedPeerUserId(result.toUserId ?? peerUserId)
    const messages = await window.api.remoteIm.listMessages(currentProjectId, 500)
    setRemoteImMessages(messages)
  }, [currentProjectId, remoteImSelectedPeerUserId])

  const handleSubmitRemoteImLogin = useCallback(async (input: RemoteImLoginSubmitInput) => {
    setRemoteImLoginSaving(true)
    setRemoteImLoginError(null)
    try {
      const accountResult = await window.api.remoteIm.setAccount(input.account)
      if (!accountResult.ok) {
        throw new Error(accountResult.error ?? '保存远程 IM 账号失败')
      }
      setRemoteImLoginState(accountResult.value)
      setRemoteImLoginRequested(true)

      if (currentProjectId) {
        const configResult = input.projectConfig
          ? await window.api.remoteIm.setConfig(currentProjectId, {
              ...input.projectConfig,
              enabled: true
            })
          : await window.api.remoteIm.getConfig(currentProjectId)
        if (configResult.ok) {
          setRemoteImConfig(configResult.value)
          setRemoteImConfigProjectId(currentProjectId)
        } else {
          showToast(configResult.error ?? '读取远程 IM 配置失败', { level: 'error' })
          return
        }
        const status = await window.api.remoteIm.getStatus(currentProjectId)
        setRemoteImStatus(status)
      }
      setShowRemoteImLogin(false)
      showToast('远程 IM 账号和项目配置已保存，正在尝试连接', { level: 'success' })
    } catch (err) {
      setRemoteImLoginError(err instanceof Error ? err.message : String(err))
    } finally {
      setRemoteImLoginSaving(false)
    }
  }, [currentProjectId])

  const handleLookupRemoteImAccount = useCallback(async (userId: string) => {
    const result = await window.api.remoteIm.getAccountByUserId(userId)
    return result.ok ? result.value?.account ?? null : null
  }, [])

  const handleRemoteImContactsSynced = useCallback((payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => {
    setRemoteImConfig(payload.config)
    setRemoteImLoginState(payload.loginState)
    if (currentProjectId) {
      setRemoteImConfigProjectId(currentProjectId)
    }
    if (!remoteImSelectedPeerUserId && payload.config.friendUserIds.length === 1) {
      setRemoteImSelectedPeerUserId(payload.config.friendUserIds[0])
    }
  }, [currentProjectId, remoteImSelectedPeerUserId])

  const handleAddRemoteImContact = useCallback(async (
    relation: RemoteImContactRelation,
    userId: string
  ) => {
    const cleanUserId = userId.trim()
    if (!cleanUserId) return
    const nextConfig = addRemoteImContact(remoteImConfig, relation, cleanUserId)
    const account: RemoteImAccountConfig = {
      provider: nextConfig.provider,
      sdkAppId: nextConfig.sdkAppId,
      desktopUserId: nextConfig.desktopUserId,
      desktopRole: nextConfig.desktopRole,
      userSigMode: nextConfig.userSigMode,
      userSigEndpoint: nextConfig.userSigEndpoint,
      userSigSecretKey: nextConfig.userSigSecretKey,
      friendUserIds: nextConfig.friendUserIds,
      masterUserIds: nextConfig.masterUserIds,
      slaveUserIds: nextConfig.slaveUserIds,
      allowedUserIds: nextConfig.allowedUserIds
    }
    const result = await window.api.remoteIm.setAccount(account)
    if (!result.ok) {
      showToast(result.error ?? '保存远程 IM 联系人失败', { level: 'error' })
      return
    }
    setRemoteImLoginState(result.value)
    setRemoteImConfig(nextConfig)
    if (currentProjectId) {
      setRemoteImConfigProjectId(currentProjectId)
    }
    setRemoteImSelectedPeerUserId(cleanUserId)
  }, [currentProjectId, remoteImConfig])

  const handleDeleteRemoteImContact = useCallback(async (userId: string) => {
    if (!currentProjectId) return
    const cleanUserId = userId.trim()
    if (!cleanUserId) return
    const result = await window.api.remoteIm.deleteContact(currentProjectId, cleanUserId)
    if (!result.ok) {
      showToast(result.error ?? '删除远程 IM 好友失败', { level: 'error' })
      return
    }
    setRemoteImConfig(result.value)
    setRemoteImLoginState(result.loginState)
    setRemoteImConfigProjectId(currentProjectId)
    setRemoteImInput('')
    if (remoteImSelectedPeerUserId === cleanUserId) {
      setRemoteImSelectedPeerUserId(null)
    }
    const messages = await window.api.remoteIm.listMessages(currentProjectId, 500)
    setRemoteImMessages(messages)
    showToast('已删除好友和聊天历史', { level: 'success' })
  }, [currentProjectId, remoteImSelectedPeerUserId])

  const handleLoadEarlierRemoteImMessages = useCallback(
    async (peerUserId: string) => {
      if (!currentProjectId || !peerUserId) return
      // 锚点 = 当前列表里该会话最早的一条；严格早于它的才是下一页。
      const peerMessages = remoteImMessages.filter(
        (message) => message.fromUserId === peerUserId || message.toUserId === peerUserId
      )
      if (peerMessages.length === 0) {
        setRemoteImEarlierExhausted((prev) => ({ ...prev, [peerUserId]: true }))
        return
      }
      const oldest = peerMessages.reduce((current, next) =>
        next.createdAt < current.createdAt ||
        (next.createdAt === current.createdAt && next.id < current.id)
          ? next
          : current
      )
      const limit = 200
      const earlier = await window.api.remoteIm.listPeerMessagesBefore(
        currentProjectId,
        peerUserId,
        { createdAt: oldest.createdAt, id: oldest.id },
        limit
      )
      if (earlier.length < limit) {
        setRemoteImEarlierExhausted((prev) => ({ ...prev, [peerUserId]: true }))
      }
      if (earlier.length > 0) {
        setRemoteImMessages((prev) => mergeRemoteImMessages(prev, earlier))
      }
    },
    [currentProjectId, remoteImMessages]
  )

  // 消息汇总 → AICLI：先把 Markdown 落成 .md 文件，再把文件路径发给当前主会话读取
  //（汇总可能有几千条消息，发路径而不是全文，避免撑爆终端输入）。
  const handleSendRemoteImSummaryToAicli = useCallback(
    async (markdown: string): Promise<boolean> => {
      if (!currentProjectId) return false
      if (!sessionId || sessionStatus !== 'running') {
        showToast('主会话未运行，无法发送给 AICLI，请先启动主会话', { level: 'warn' })
        return false
      }
      const saved = await window.api.remoteIm.saveSummaryMarkdown(currentProjectId, markdown)
      if (!saved.ok) {
        showToast(saved.error ?? '保存消息汇总文件失败', { level: 'error' })
        return false
      }
      const sendResult = await window.api.cc.sendUser(
        sessionId,
        `这是一份「时光胶囊」——此前的 IM 消息记录汇总（Markdown 文件）。请阅读它，找回对话记忆与背景：\n${saved.path}`
      )
      if (!sendResult.ok) {
        showToast(sendResult.error ?? '时光胶囊开启失败', { level: 'error' })
        return false
      }
      // 正常流程静默：不弹任何提示，AICLI 终端里能直接看到胶囊消息进入处理。
      return true
    },
    [currentProjectId, sessionId, sessionStatus]
  )

  const handleSendRuntimeLog = useCallback(async (comment = '') => {
    if (!runtimeState.projectId || !runtimeState.log.trim()) {
      showToast('当前没有可发送的运行日志', { level: 'warn' })
      return
    }
    if (!sessionId || sessionStatus !== 'running') {
      showToast('主会话未运行，无法发送运行日志，请先启动主会话', { level: 'warn' })
      return
    }

    const promptResult = await window.api.runtime.getAnalysisPromptFile()
    if (!promptResult.ok) {
      showToast(promptResult.error ?? '获取运行日志分析提示失败', { level: 'error' })
      return
    }

    const message = buildRuntimeLogAnalysisMessage(promptResult.message, comment)
    const sendResult = await window.api.cc.sendUser(sessionId, message)
    if (!sendResult.ok) {
      showToast(sendResult.error ?? '发送运行日志失败', { level: 'error' })
      return
    }

    setRuntimeLogComment((currentComment) =>
      nextRuntimeLogCommentAfterSendResult(currentComment, sendResult.ok)
    )
    setShowRuntimeLogDialog((currentOpen) =>
      nextRuntimeLogDialogOpenAfterSendResult(currentOpen, sendResult.ok)
    )
    showToast('已将运行日志分析请求发送到主会话', { level: 'success' })
  }, [runtimeState.log, runtimeState.projectId, sessionId, sessionStatus])

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
   * user is expected to pick a new/resume action again from the gate.
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

  const onWorkModeSelect = useCallback(
    (value: WorkMode) => {
      if (value === workMode) return
      if (sessionStatus === 'running') {
        alert('会话正在运行，请先停止（Kill）后再切换模式。')
        return
      }
      setWorkMode(value)
      setDiffAnnotations([])
      setDiffGeneralNote('')
      if (value === 'task-watch') {
        setPlanReview(null)
        setDiffReviewOpen(false)
        setShowNormalTaskDialog(false)
      } else {
        setShowScheduledTaskDialog(false)
      }
    },
    [workMode, sessionStatus]
  )

  const runNormalTask = useCallback(
    async (task: NormalTaskEntry): Promise<void> => {
      if (!sessionId || sessionStatus !== 'running') {
        showToast('请先启动 AICLI', { level: 'warn' })
        return
      }
      if (!targetRepo) {
        showToast('当前项目未设置 target_repo', { level: 'warn' })
        return
      }
      if (task.name !== planName) {
        setDiffAnnotations([])
        setDiffGeneralNote('')
        setPlanReview(null)
        setPlanName(task.name)
      }

      const prompt = buildNormalTaskRunPrompt(task, targetRepo)
      const sendResult = await window.api.cc.sendUser(sessionId, prompt)
      if (!sendResult.ok) {
        showToast(sendResult.error ?? '发送普通任务失败', { level: 'error' })
        return
      }

      showToast(`已发送普通任务「${task.name}」到 AICLI`, { level: 'success' })
    },
    [planName, sessionId, sessionStatus, targetRepo]
  )

  const persistNormalTaskMetadata = useCallback(
    async (
      name: string,
      metadata: NormalTaskMetadataDraft
    ): Promise<{ ok: true } | { ok: false; error: string }> => {
      if (!projectDir) return { ok: false, error: '请先打开一个项目' }
      const request = {
        projectDir,
        name,
        description: metadata.description,
        details: metadata.details
      }
      if (typeof window.api.plan.updateMetadata === 'function') {
        try {
          return await window.api.plan.updateMetadata(request)
        } catch (err: unknown) {
          const fallback = await window.api.plan.updateDescription(request)
          if (fallback.ok) return fallback
          return {
            ok: false,
            error: err instanceof Error ? err.message : String(err)
          }
        }
      }
      return await window.api.plan.updateDescription(request)
    },
    [projectDir]
  )

  const createNormalTask = useCallback(
    async (name: string, description = '', details = ''): Promise<NormalTaskEntry | null> => {
      if (!projectDir) {
        showToast('请先打开一个项目', { level: 'warn' })
        return null
      }
      const result = await window.api.plan.createInternal({ projectDir, name })
      if (!result.ok) {
        showToast(result.error, { level: 'error' })
        return null
      }
      const created: NormalTaskEntry = {
        name: result.name,
        abs: result.abs,
        source: 'internal'
      }
      const nextDescription = description.trim()
      const nextDetails = details.trim()
      if (nextDescription || nextDetails) {
        const metadataResult = await persistNormalTaskMetadata(result.name, {
          description: nextDescription,
          details: nextDetails
        })
        if (metadataResult.ok) {
          if (nextDescription) created.description = nextDescription
          if (nextDetails) created.details = nextDetails
        } else {
          showToast(metadataResult.error, { level: 'error' })
        }
      }
      knownPlanNamesRef.current.add(created.name)
      setPlanList((current) =>
        [...current.filter((task) => task.name !== created.name), created].sort((a, b) =>
          a.name.localeCompare(b.name)
        )
      )
      void refreshPlanList()
      setPlanName(result.name)
      setDiffAnnotations([])
      setDiffGeneralNote('')
      setPlanReview(null)
      showToast(`已创建普通任务「${result.name}」`, { level: 'success' })
      return created
    },
    [persistNormalTaskMetadata, projectDir, refreshPlanList]
  )

  const saveNormalTaskMetadata = useCallback(
    async (name: string, metadata: NormalTaskMetadataDraft): Promise<boolean> => {
      if (!projectDir) {
        showToast('请先打开一个项目', { level: 'warn' })
        return false
      }
      const result = await persistNormalTaskMetadata(name, metadata)
      if (!result.ok) {
        showToast(result.error, { level: 'error' })
        return false
      }
      const nextDescription = metadata.description.trim()
      const nextDetails = metadata.details.trim()
      const refreshed = await refreshPlanList()
      if (!verifyNormalTaskMetadataSaved(refreshed, name, metadata)) {
        showToast('普通任务详情没有写入，请重启应用后重试。', { level: 'error' })
        return false
      }
      setPlanList((current) =>
        current.map((task) =>
          task.name === name
            ? {
                ...task,
                description: nextDescription || undefined,
                details: nextDetails || undefined
              }
            : task
        )
      )
      showToast('已保存普通任务信息', { level: 'success' })
      return true
    },
    [persistNormalTaskMetadata, projectDir, refreshPlanList]
  )

  const autoSaveNormalTaskMetadata = useCallback(
    async (name: string, metadata: NormalTaskMetadataDraft): Promise<boolean> => {
      if (!projectDir) return false
      const result = await persistNormalTaskMetadata(name, metadata)
      if (!result.ok) return false
      const nextDescription = metadata.description.trim()
      const nextDetails = metadata.details.trim()
      setPlanList((current) =>
        current.map((task) =>
          task.name === name
            ? {
                ...task,
                description: nextDescription || undefined,
                details: nextDetails || undefined
              }
            : task
        )
      )
      return true
    },
    [persistNormalTaskMetadata, projectDir]
  )

  /** Load the current plan md and open the review + annotation dialog. */
  const openPlanReview = useCallback(async (name = planName) => {
    if (!projectDir) return
    const currentName = name.trim()
    if (!currentName) {
      showToast('请先选择普通任务', { level: 'warn' })
      return
    }
    const res = await window.api.artifact.readCurrent(
      projectDir,
      1,
      currentName
    )
    if (!res.ok || res.content === undefined) {
      // Special case: the current plan is external (registered via import)
      // but its source file was deleted. Offer to remove the stale mapping
      // instead of the generic "let AI design" hint.
      const currentPlan = planList.find((p) => p.name === currentName)
      const isDeadExternal =
        currentPlan?.source === 'external' && res.path
          ? /ENOENT|no such file|不存在|找不到|not.*exist/i.test(
              res.error ?? ''
            )
          : false
      if (isDeadExternal && currentProjectId && currentName) {
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
                  name: currentName
                })
                if (!rm.ok) {
                  showToast(`移除失败：${rm.error}`, { level: 'error' })
                  return
                }
                setPlanName('')
                setPlanList((prev) => prev.filter((p) => p.name !== currentName))
                knownPlanNamesRef.current.delete(currentName)
                showToast('已从普通任务列表移除', { level: 'info' })
              }
            }
          }
        )
        return
      }
      showToast(
        `暂无可预览的普通任务文档${res.path ? `（${res.path}）` : ''}。`,
        { level: 'warn' }
      )
      return
    }
    setPlanReview({ path: res.path ?? '', content: res.content })
  }, [projectDir, planName, planList, currentProjectId])

  /** Open the git diff viewer. */
  const openDiffReview = useCallback(() => {
    if (!targetRepo) {
      showToast('本项目没有 target_repo 路径，无法打开代码审查', { level: 'warn' })
      return
    }
    if (noPlanMode) {
      showToast('定时任务下暂不支持代码审查批注，请先切换到普通任务。', { level: 'warn' })
      return
    }
    setDiffReviewOpen(true)
  }, [targetRepo, noPlanMode])

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
      if (noPlanMode) {
        showToast('定时任务下没有方案文件，无法发送代码审查批注。', { level: 'warn' })
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
      // Sent successfully — clear the batch so the next 代码审查 starts fresh.
      setDiffAnnotations([])
      setDiffGeneralNote('')
      setDiffReviewOpen(false)
      showToast(`已发送 ${anns.length} 条批注到会话`, { level: 'info' })
    },
    [sessionId, sessionStatus, noPlanMode, planName, getPlanAbsPath, currentProjectId, targetRepo]
  )

  const judgeExternalReviewItem = useCallback(
    async (suggestion: ExternalReviewSuggestion) => {
      if (!sessionId || sessionStatus !== 'running') {
        return { ok: false as const, error: 'session not running' }
      }
      if (noPlanMode) {
        return { ok: false as const, error: 'no plan selected' }
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
    [sessionId, sessionStatus, noPlanMode, getPlanAbsPath, planName]
  )

  const globalSearchQuickActions = [
    {
      title: 'Prompt 模板',
      snippet: '管理和插入常用 prompt 模板。',
      location: '次级入口',
      onOpen: () => setShowTemplates(true)
    },
    {
      title: '新手向导',
      snippet: '重新打开新手上手向导。',
      location: '次级入口',
      onOpen: () => setShowOnboarding(true)
    }
  ]

  const commandPaletteCommands: Command[] = [
    {
      id: 'proj.picker',
      label: '📁 项目管理（切换 / 新建 / 删除）',
      keywords: 'project switch new',
      action: () => setShowProjectPicker(true)
    },
    {
      id: 'settings',
      label: '⚙️ 设置',
      keywords: 'settings command ai cli',
      action: () => openAiSettingsSection()
    },
    {
      id: 'tpl',
      label: '📋 Prompt 模板',
      keywords: 'templates prompt snippets',
      action: () => setShowTemplates(true)
    },
    {
      id: 'onboard',
      label: '❓ 新手向导',
      keywords: 'help onboarding wizard',
      action: () => setShowOnboarding(true)
    },
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
      disabled: !hasProject || noPlanMode || !planName.trim()
    },
    {
      id: 'diff-review',
      label: '🔀 代码审查',
      keywords: 'code review diff annotate',
      action: () => void openDiffReview(),
      disabled: !hasProject || noPlanMode
    }
  ]


  return (
    <div className={`app platform-${window.api.platform}`}>
      <header className="topbar">
        <div className="topbar-left">
          {window.api.platform === 'darwin' && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="violet"
              onClick={() => window.api.launchNewInstance()}
              title="新建应用实例：打开一个相互隔离的 Multi-AI Code 窗口"
              aria-label="新建应用实例"
            >
              ＋
            </button>
          )}
          <button
            className="topbar-btn topbar-btn-primary topbar-btn-icon"
            onClick={() => void openProjectDirPicker()}
            title={
              hasProject
                ? `当前项目：${projectName}（点击浏览打开其他项目文件夹）`
                : '浏览打开项目文件夹'
            }
            aria-label={hasProject ? `当前项目：${projectName}` : '选择项目'}
          >
            📁
          </button>
          <span className="meta">
            {hasProject ? (
              <code title={targetRepo}>{targetRepo}</code>
            ) : (
              <span className="meta-warn">⚠ 未选择项目（请点左上角「📁 选择项目」）</span>
            )}
          </span>
          {hasProject && (
            <span
              className="topbar-session"
              title={`会话状态：${mainSessionStatusLabel}`}
            >
              <span
                className={`main-session-dot ${sessionStatus}`}
                aria-label={mainSessionStatusLabel}
              />
              <span className="topbar-session-plan">{mainSessionPlanLabel}</span>
            </span>
          )}
        </div>
        <div className="topbar-actions">
          <button
            className="topbar-btn topbar-btn-icon"
            data-tone="blue"
            onClick={() => openAiSettingsSection()}
            title="设置：全局截图快捷键、AI CLI 命令 / 参数 / 环境变量"
            aria-label="设置"
          >
            ⚙️
          </button>
          <button
            className={`topbar-btn remote-im-topbar remote-im-topbar-${remoteImStatus?.state ?? 'disconnected'}`}
            onClick={() => setShowRemoteImDrawer(true)}
            disabled={!currentProjectId}
            title={
              remoteImStatus?.detail
                ? `远程 IM：${getRemoteImStatusLabel(remoteImStatus)} - ${remoteImStatus.detail}`
                : `远程 IM：${getRemoteImStatusLabel(remoteImStatus)}`
            }
            aria-label={`远程 IM：${getRemoteImStatusLabel(remoteImStatus)}`}
          >
            <span className="remote-im-topbar-dot" />
            💬
          </button>
          <button
            className="topbar-btn topbar-btn-icon"
            data-tone="blue"
            onClick={() => setShowRemoteImSummary(true)}
            disabled={!currentProjectId}
            title="消息汇总：把当前项目的全部 IM 消息记录汇总为 Markdown 展示"
            aria-label="消息汇总"
          >
            🗒
          </button>
          {hasProject && appSettings.showDevToolbarButtons && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="amber"
              onClick={() => setShowBuildPanel(true)}
              disabled={!currentProjectId}
              title="构建：打开项目构建面板"
              aria-label="构建"
            >
              🔨
            </button>
          )}
          {hasProject && appSettings.showDevToolbarButtons && (
            <button
              className={`topbar-btn topbar-btn-icon${runtimeTopbarRunning ? ' is-active' : ''}`}
              data-tone="success"
              onClick={() => {
                if (runtimeTopbarRunning) {
                  setShowRuntimeLogDialog(true)
                } else {
                  void handleStartRuntime()
                }
              }}
              disabled={runtimeTopbarDisabled}
              title={
                runtimeTopbarRunning
                  ? '运行中：打开运行日志'
                  : runtimeStartBlockedReason ?? '运行：启动项目运行并打开实时日志'
              }
              aria-label={runtimeTopbarRunning ? '运行中，打开运行日志' : '运行'}
            >
              ▶
            </button>
          )}
          {hasProject && isPlanDesignMode && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="blue"
              onClick={() => setShowNormalTaskDialog(true)}
              disabled={!currentProjectId}
              title="普通任务：创建、选择和查看"
              aria-label="普通任务"
            >
              📋
            </button>
          )}
          {hasProject && isTaskWatchMode && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="amber"
              onClick={() => setShowScheduledTaskDialog(true)}
              disabled={!currentProjectId}
              title="定时任务：创建和管理到点后交给当前 AICLI 执行的任务"
              aria-label="定时任务"
            >
              ⏰
            </button>
          )}
          {hasProject && isPlanDesignMode && planName.trim() && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="blue"
              onClick={() => void openPlanReview()}
              disabled={!currentProjectId}
              title="方案预览：查看 / 标注当前普通任务的方案文档"
              aria-label="方案预览"
            >
              📄
            </button>
          )}
          {mainPanelMounted && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="blue"
              onClick={() => void openDiffReview()}
              disabled={!canStartCurrentMainSession || noPlanMode || sessionStatus !== 'running'}
              title="代码审查：打开 diff 并把批注回灌给当前会话"
              aria-label="代码审查"
            >
              <FileDiffIcon size={18} verticalAlign="middle" />
            </button>
          )}
          {mainPanelMounted &&
            (sessionStatus === 'running' ? (
              <button
                className="topbar-btn topbar-btn-icon"
                data-tone="danger"
                onClick={handleStop}
                disabled={!canStartCurrentMainSession}
                title="停止当前主会话"
                aria-label="停止当前主会话"
              >
                ⏹
              </button>
            ) : (
              <button
                className="topbar-btn topbar-btn-icon"
                data-tone="success"
                onClick={
                  sessionStatus === 'exited' ? handleRestart : () => void handleStart('new')
                }
                disabled={!canStartCurrentMainSession}
                title={sessionStatus === 'exited' ? '重启主会话' : '启动主会话'}
                aria-label={sessionStatus === 'exited' ? '重启主会话' : '启动主会话'}
              >
                {sessionStatus === 'exited' ? '🔄' : '▶'}
              </button>
            ))}
          <button
            className="topbar-btn mode-toggle-btn"
            data-tone="violet"
            onClick={() => onWorkModeSelect(isTaskWatchMode ? 'plan-design' : 'task-watch')}
            title={
              sessionStatus === 'running'
                ? '运行中无法切换模式，请先停止'
                : isTaskWatchMode
                  ? '当前：定时任务，点击切换到普通任务'
                  : '当前：普通任务，点击切换到定时任务'
            }
            aria-label={isTaskWatchMode ? '切换到普通任务' : '切换到定时任务'}
            aria-pressed={isTaskWatchMode}
          >
            🔀
          </button>
          <button
            className="topbar-btn topbar-btn-icon"
            data-tone="amber"
            onClick={handleToggleTheme}
            title={theme === 'dark' ? '切换到浅色' : '切换到暗色'}
            aria-label="切换主题"
          >
            {theme === 'dark' ? '☀' : '☾'}
          </button>
          {appSettings.showDevToolbarButtons && (
            <button
              className={`topbar-btn ${errorCount > 0 ? 'topbar-btn-danger' : 'topbar-btn-icon'}`}
              onClick={() => setShowErrors((s) => !s)}
              title="日志：查看错误与通知"
              aria-label={errorCount > 0 ? `日志（${errorCount} 条错误/警告）` : '日志'}
            >
              {errorCount > 0 ? `⚠ ${errorCount}` : '📣'}
            </button>
          )}
        </div>
      </header>

      <div className="main-split">
        <div className="main-column">
          {mainPanelMounted ? (
            <MainPanel
              sessionId={sessionId ?? ''}
              projectId={currentProjectId ?? ''}
              projectDir={projectDir}
              cwd={targetRepo}
              aiCli={aiSettings.ai_cli ?? DEFAULT_AI_CLI}
              onOpenRepoView={() => void openRepoView()}
              repoViewDisabled={!currentProjectId}
            />
          ) : (
            <MainBootGate
              phase={gatePhase}
              command={aiSettings.command ?? aiSettings.ai_cli ?? DEFAULT_AI_CLI}
              workMode={isTaskWatchMode ? 'scheduled-task' : 'normal-task'}
              disabled={!canStartCurrentMainSession}
              onChoose={(mode) => void handleStart(mode)}
              onDismissFailure={() => setGatePhase({ kind: 'idle' })}
            />
          )}
        </div>

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
          }}
        />
      )}
      {showNormalTaskDialog && isPlanDesignMode && currentProjectId && (
        <NormalTaskDialog
          tasks={planList}
          selectedName={planName}
          sessionRunning={sessionStatus === 'running'}
          onCreate={createNormalTask}
          onSelect={setPlanName}
          onRun={runNormalTask}
          onPreview={(name) => void openPlanReview(name)}
          onSaveMetadata={saveNormalTaskMetadata}
          onAutoSaveMetadata={autoSaveNormalTaskMetadata}
          onRefresh={async () => {
            await refreshPlanList()
          }}
          onClose={() => setShowNormalTaskDialog(false)}
        />
      )}
      {showScheduledTaskDialog && isTaskWatchMode && currentProjectId && (
        <ScheduledTaskDialog
          onClose={() => setShowScheduledTaskDialog(false)}
          projectId={currentProjectId}
          targetRepo={targetRepo}
          sessionId={sessionId}
          sessionRunning={sessionStatus === 'running'}
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
          initialRuntimeConfig={visibleProjectRuntimeConfig}
          runtimeConfigReady={projectRuntimeConfigReady}
          runtimeConfigDisabled={runtimeStateForCurrentProject.status === 'running'}
          visualStudioInstallations={visualStudioInstallations}
          visualStudioInstallationsLoading={visualStudioInstallationsLoading}
          onRefreshVisualStudioInstallations={() => {
            void refreshVisualStudioInstallations()
          }}
          initialSection={aiSettingsInitialSection}
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
          onSavedRuntimeConfig={(next) => {
            setProjectRuntimeConfig(next)
            setProjectRuntimeConfigProjectId(currentProjectId)
          }}
        />
      )}
      <RemoteImClientHost
        projectId={currentProjectId}
        config={remoteImConfig}
        loginRequested={remoteImLoginRequested}
        onContactsSynced={handleRemoteImContactsSynced}
      />
      <RemoteImDrawer
        canLoadEarlier={
          remoteImSelectedPeerUserId
            ? !remoteImEarlierExhausted[remoteImSelectedPeerUserId]
            : false
        }
        onLoadEarlier={(peerUserId) => handleLoadEarlierRemoteImMessages(peerUserId)}
        open={showRemoteImDrawer}
        projectId={currentProjectId}
        sessionRunning={sessionStatus === 'running'}
        status={remoteImStatus}
        config={remoteImConfig}
        messages={remoteImMessages}
        selectedPeerUserId={remoteImSelectedPeerUserId}
        input={remoteImInput}
        onInputChange={setRemoteImInput}
        onSelectPeer={setRemoteImSelectedPeerUserId}
        onSend={(toUserId) => void handleSendRemoteImLocalMessage(toUserId)}
        onSendImage={(toUserId, file) => void handleSendRemoteImImage(toUserId, file)}
        onAddContact={(relation, userId) => void handleAddRemoteImContact(relation, userId)}
        onDeleteContact={(userId) => void handleDeleteRemoteImContact(userId)}
        onClose={() => setShowRemoteImDrawer(false)}
      />
      <RemoteImSummaryDialog
        open={showRemoteImSummary}
        projectId={currentProjectId}
        ownerUserId={remoteImLoginState?.account.desktopUserId ?? null}
        canSendToAicli={Boolean(sessionId) && sessionStatus === 'running'}
        onSendToAicli={handleSendRemoteImSummaryToAicli}
        onClose={() => setShowRemoteImSummary(false)}
      />
      <RemoteImLoginDialog
        open={showRemoteImLogin}
        loginState={remoteImLoginState}
        projectConfig={currentProjectId && remoteImConfigReady ? remoteImConfig : null}
        projectConfigReady={remoteImConfigReady}
        saving={remoteImLoginSaving}
        error={remoteImLoginError}
        onLookupAccount={handleLookupRemoteImAccount}
        onClose={() => setShowRemoteImLogin(false)}
        onSubmit={(input) => void handleSubmitRemoteImLogin(input)}
      />
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
          onStartBuild={() => void handleStartBuild('all')}
          onStartSingleBuild={(stepId) => void handleStartBuild('single-step', stepId)}
          onStopBuild={() => void handleStopBuild()}
          onAnalyzeFailure={() => void handleAnalyzeBuildFailure()}
        />
      )}
      {showRuntimeLogDialog && (
        <RuntimeLogDialog
          open={showRuntimeLogDialog}
          currentProjectId={visibleRuntimeState.projectId ?? currentProjectId}
          currentProjectName={visibleRuntimeState.projectName ?? (projectName || null)}
          runtimeState={visibleRuntimeState}
          sessionId={sessionId}
          sessionStatus={sessionStatus}
          comment={runtimeLogComment}
          onCommentChange={setRuntimeLogComment}
          onClose={() => setShowRuntimeLogDialog(false)}
          onStopRuntime={() => void handleStopRuntime()}
          onSendRuntimeLog={(comment) => void handleSendRuntimeLog(comment)}
        />
      )}
      {showErrors && <ErrorPanel onClose={() => setShowErrors(false)} />}

      <ToastHost />

    </div>
  )
}
