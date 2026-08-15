import { useCallback, useEffect, useRef, useState } from 'react'
import { FileDiffIcon } from '@primer/octicons-react'
import { getTheme, toggleTheme } from './utils/theme.js'
import { formatAnnotationsForSession } from './utils/session-message-format'
import { buildCliLaunchArgs } from './utils/cliLaunchArgs'
import { canStartMainSession } from './utils/mainSessionPlanMode'
import MainPanel from './components/MainPanel'
import MainBootGate, { type BootGatePhase } from './components/MainBootGate'
import type { ProjectInfo } from './types/project'
import ErrorPanel, { pushLog, useLogs } from './components/ErrorPanel'
import AiSettingsDialog, {
  DEFAULT_AI_CLI,
  DEFAULT_AI_PERMISSION_MODE,
  type AiSettings
} from './components/AiSettingsDialog'
import TemplatesDialog from './components/TemplatesDialog'
import ScheduledTaskDialog from './scheduled-tasks/ScheduledTaskDialog'
import RemoteImDrawer from './remote-im/RemoteImDrawer'
import RemoteImSummaryDialog from './remote-im/RemoteImSummaryDialog'
import WindowControls from './components/WindowControls'
import { mergeRemoteImMessages } from './remote-im/messageMerge'
import RemoteImClientHost from './remote-im/RemoteImClientHost'
import RemoteDesktopSharingBar from './remote-desktop/RemoteDesktopSharingBar'
import type { RemoteDesktopControllerState } from '../electron/remote-desktop/controller.js'
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
import CommandPalette, { type Command } from './components/CommandPalette'
import ToastHost, { showToast } from './components/Toast'
import GlobalSearchDialog from './components/GlobalSearchDialog'
import DiffViewerDialog, { type DiffAnnotation } from './components/DiffViewerDialog'
import type { DiffMode } from './components/diffViewerConfig'
import type { ExternalReviewSuggestion } from './components/externalAiReview'
import type {
  RemoteImAccountConfig,
  RemoteImConfig,
  RemoteImContactRelation,
  RemoteImLoginState,
  RemoteImMessage,
  RemoteImStatus
} from '../electron/preload'

const LAST_PROJECT_KEY = 'multi-ai-code.lastProjectId'
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
  outputMaxChunkChars: 4000,
  remoteDesktopMode: 'disabled'
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
  const [showErrors, setShowErrors] = useState(false)
  const [showAiSettings, setShowAiSettings] = useState(false)
  const [aiSettings, setAiSettings] = useState<AiSettings>({ ai_cli: DEFAULT_AI_CLI })
  const [repoViewAiSettings, setRepoViewAiSettings] = useState<AiSettings>({
    ai_cli: DEFAULT_AI_CLI
  })
  const [aiSettingsReady, setAiSettingsReady] = useState(false)
  const [aiSettingsLoadError, setAiSettingsLoadError] = useState<string | null>(null)
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
  // 被控端共享状态。RemoteImClientHost 是 headless 的，指示条在这里渲染。
  const [remoteDesktopState, setRemoteDesktopState] = useState<RemoteDesktopControllerState>({
    hostState: 'idle',
    peerUserId: null,
    sessionId: null
  })
  const remoteDesktopStopRef = useRef<(() => Promise<void>) | null>(null)
  const [showRemoteImLogin, setShowRemoteImLogin] = useState(false)
  const [remoteImLoginSaving, setRemoteImLoginSaving] = useState(false)
  const [remoteImLoginError, setRemoteImLoginError] = useState<string | null>(null)
  const [showTemplates, setShowTemplates] = useState(false)
  const [showScheduledTaskDialog, setShowScheduledTaskDialog] = useState(false)
  const [showRemoteImDrawer, setShowRemoteImDrawer] = useState(false)
  const [showRemoteImSummary, setShowRemoteImSummary] = useState(false)
  const [showCmdk, setShowCmdk] = useState(false)
  const [showGlobalSearch, setShowGlobalSearch] = useState(false)
  const [theme, setThemeState] = useState<'light' | 'dark'>(() => getTheme())

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
  const [msysEnabled, setMsysEnabled] = useState(false)
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
  // 当前工作空间是不是 git 仓库。看 diff 的唯一前提就是这个——与是否选了普通任务、
  // 会话是否在跑都无关。null = 还没探测出结果（按钮先不禁用，避免闪一下灰）。
  const [isGitRepo, setIsGitRepo] = useState<boolean | null>(null)
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

  /** Clear UI state tied to a specific project (drawers, dialogs). */
  const clearProjectScopedState = useCallback(() => {
    setShowGlobalSearch(false)
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
    void (async () => {
      const list = await reloadProjects()
      const last = localStorage.getItem(LAST_PROJECT_KEY)
      const pick = list.find((p) => p.id === last) ?? list[0]
      // 没有项目时不自动弹任何东西：顶栏那条工作空间选择器本身就写着
      // 「选择工作空间」，再弹一层只是把同一句话重说一遍，还挡着别的操作。
      if (pick) {
        setCurrentProjectId(pick.id)
        if (last && last !== pick.id) {
          showToast(`上次打开的项目已不存在，已切换到「${pick.name}」`, { level: 'warn' })
        }
      }
    })()
  }, [reloadProjects])

  const currentProject = projects.find((p) => p.id === currentProjectId) ?? null
  const projectDir = currentProject?.dir ?? ''
  const targetRepo = currentProject?.target_repo ?? ''
  const projectName = currentProject?.name ?? ''
  const hasProject = currentProject !== null

  // 探测工作空间是否是 git 仓库。复用 git:status——非仓库时它返回 ok:false，
  // 不需要为此新开一个 IPC。
  useEffect(() => {
    if (!targetRepo) {
      setIsGitRepo(null)
      return
    }
    let cancelled = false
    setIsGitRepo(null)
    void window.api.git.status(targetRepo).then((res) => {
      if (!cancelled) setIsGitRepo(res.ok)
    })
    return () => {
      cancelled = true
    }
  }, [targetRepo])
  const canStartCurrentMainSession = canStartMainSession(currentProjectId)
  const mainSessionStatusLabel =
    sessionStatus === 'running' ? '运行中' : sessionStatus === 'exited' ? '已退出' : '待启动'

  useEffect(() => {
    if (!currentProjectId) {
      setStageConfigs({})
      setMsysEnabled(false)
      setAiSettings({ ai_cli: DEFAULT_AI_CLI })
      setRepoViewAiSettings({ ai_cli: DEFAULT_AI_CLI })
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

      if (aiResult.repaired || repoResult.repaired) {
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

  // 任务不分模式，进项目就扫一次。
  useEffect(() => {
    if (!currentProjectId) return
    void window.api.scheduledTasks.scanNow(currentProjectId)
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

  const handleStart = useCallback(async (mode: 'new' | 'resume' = 'new') => {
    if (!currentProjectId || !canStartMainSession(currentProjectId)) return
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
      aiSettings.args ?? [],
      aiSettings.permission_mode ?? DEFAULT_AI_PERMISSION_MODE
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
    // 会话一起来就扫一次：定时任务此前只在"定时任务模式"下才会被调度，
    // 普通模式下到点也不跑。现在任何会话都能接任务。
    void window.api.scheduledTasks.scanNow(currentProjectId)
  }, [currentProjectId, projects, aiSettings, aiSettingsReady, aiSettingsLoadError])

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
    const result = await window.api.remoteIm.sendPeerImage(currentProjectId, file, {
      fileToken,
      toUserId: peerUserId,
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
      allowedUserIds: nextConfig.allowedUserIds,
      // Explicitly adding a contact is the only action that removes its local
      // revoke tombstone. Other blocked SDK friends remain blocked.
      blockedUserIds: (remoteImLoginState?.account.blockedUserIds ?? []).filter(
        (userId) => userId !== cleanUserId
      )
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
  }, [currentProjectId, remoteImConfig, remoteImLoginState?.account.blockedUserIds])

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

  /** Open the git diff viewer. */
  const openDiffReview = useCallback(() => {
    if (!targetRepo) {
      showToast('本项目没有 target_repo 路径，无法打开代码审查', { level: 'warn' })
      return
    }
    if (isGitRepo === false) {
      showToast('当前工作空间不是 git 仓库，无法查看 diff', { level: 'warn' })
      return
    }
    setDiffReviewOpen(true)
  }, [targetRepo, isGitRepo])

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
      const text = formatAnnotationsForSession({
        annotations: anns,
        generalComment: generalNote
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
    [sessionId, sessionStatus]
  )

  const judgeExternalReviewItem = useCallback(
    async (suggestion: ExternalReviewSuggestion) => {
      if (!sessionId || sessionStatus !== 'running') {
        return { ok: false as const, error: 'session not running' }
      }
      return window.api.cc.judgeExternalReview({
        sessionId,
        planAbsPath: '',
        suggestion: {
          rawText: suggestion.rawText,
          pathHint: suggestion.pathHint,
          lineHint: suggestion.lineHint,
          linkedDiffFile: suggestion.linkedDiffFile
        }
      })
    },
    [sessionId, sessionStatus]
  )

  const globalSearchQuickActions = [
    {
      title: 'Prompt 模板',
      snippet: '管理和插入常用 prompt 模板。',
      location: '次级入口',
      onOpen: () => setShowTemplates(true)
    },
  ]

  const commandPaletteCommands: Command[] = [
    {
      id: 'proj.picker',
      label: '📁 选择工作空间（切换 / 新建）',
      keywords: 'workspace project switch new open folder',
      action: () => void openProjectDirPicker()
    },
    {
      id: 'settings',
      label: '⚙️ 设置',
      keywords: 'settings command ai cli',
      action: () => setShowAiSettings(true)
    },
    {
      id: 'tpl',
      label: '📋 Prompt 模板',
      keywords: 'templates prompt snippets',
      action: () => setShowTemplates(true)
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
      id: 'diff-review',
      label: '🔀 代码审查',
      keywords: 'code review diff annotate',
      action: () => void openDiffReview(),
      disabled: !hasProject || isGitRepo === false
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
          {/* 工作空间选择器：未选时就是行动号召，选后原地变成完整仓库路径。
              显示全路径而不是目录名——同名目录在不同盘/父目录下很常见，只看
              末段分不出是哪一个。 */}
          <button
            className="workspace-picker"
            onClick={() => void openProjectDirPicker()}
            title={
              hasProject
                ? `当前工作空间：${targetRepo}（点击切换）`
                : '选择一个代码仓库目录作为工作空间'
            }
            aria-label={
              hasProject ? `当前工作空间：${targetRepo}，点击切换` : '选择工作空间'
            }
          >
            <svg
              className="workspace-picker-icon"
              viewBox="0 0 16 16"
              fill="none"
              stroke="currentColor"
              strokeWidth="1.3"
              strokeLinejoin="round"
              aria-hidden="true"
            >
              <path d="M1.9 4.4c0-.72.58-1.3 1.3-1.3h2.63c.34 0 .67.14.91.38l.84.84h4.22c.72 0 1.3.58 1.3 1.3v5.98c0 .72-.58 1.3-1.3 1.3H3.2c-.72 0-1.3-.58-1.3-1.3V4.4Z" />
            </svg>
            <span className="workspace-picker-name">
              {hasProject ? targetRepo : '选择工作空间'}
            </span>
            <span className="workspace-picker-chevron" aria-hidden="true">
              ›
            </span>
          </button>
          {/* 顶栏只报会话状态，不写当前选了哪个普通任务——当前任务是内部状态，
              打开任务弹窗就能看到，常驻在顶栏没有必要。 */}
          {hasProject && (
            <span
              className="topbar-session"
              title={`会话状态：${mainSessionStatusLabel}`}
            >
              <span
                className={`main-session-dot ${sessionStatus}`}
                aria-label={mainSessionStatusLabel}
              />
            </span>
          )}
        </div>
        <div className="topbar-actions">
          <button
            className="topbar-btn topbar-btn-icon"
            data-tone="blue"
            onClick={() => setShowAiSettings(true)}
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
          {hasProject && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="amber"
              onClick={() => setShowScheduledTaskDialog(true)}
              disabled={!currentProjectId}
              title="任务管理：创建手动任务与定时任务，交给当前 AICLI 执行"
              aria-label="任务管理"
            >
              ⏰
            </button>
          )}
          {/* 看 diff 的唯一前提是「当前工作空间是 git 仓库」。既不要求选中普通任务，
              也不要求会话在跑——那两个是**回灌批注**的前提，由 submitDiffAnnotations
              自己守卫并提示。以前按钮挂在 mainPanelMounted 下，会话没起来时整个不出现。 */}
          {hasProject && (
            <button
              className="topbar-btn topbar-btn-icon"
              data-tone="blue"
              onClick={() => void openDiffReview()}
              disabled={isGitRepo === false}
              title={
                isGitRepo === false
                  ? '当前工作空间不是 git 仓库，无法查看 diff'
                  : '代码审查：查看 diff（可把批注回灌给运行中的会话）'
              }
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
            className="topbar-btn topbar-btn-icon"
            data-tone="amber"
            onClick={handleToggleTheme}
            title={theme === 'dark' ? '切换到浅色' : '切换到暗色'}
            aria-label="切换主题"
          >
            {theme === 'dark' ? '☀' : '☾'}
          </button>
        </div>
        <WindowControls />
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
              disabled={!canStartCurrentMainSession}
              onChoose={(mode) => void handleStart(mode)}
              onDismissFailure={() => setGatePhase({ kind: 'idle' })}
            />
          )}
        </div>

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
      {showScheduledTaskDialog && currentProjectId && (
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
        />
      )}
      <RemoteImClientHost
        projectId={currentProjectId}
        config={remoteImConfig}
        loginRequested={remoteImLoginRequested}
        onContactsSynced={handleRemoteImContactsSynced}
        onRemoteDesktopStateChanged={setRemoteDesktopState}
        onRemoteDesktopHostReady={(stop) => {
          remoteDesktopStopRef.current = stop
        }}
      />
      <RemoteDesktopSharingBar
        state={remoteDesktopState}
        onStop={() => {
          void remoteDesktopStopRef.current?.()
        }}
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
      {showErrors && <ErrorPanel onClose={() => setShowErrors(false)} />}

      <ToastHost />

    </div>
  )
}
