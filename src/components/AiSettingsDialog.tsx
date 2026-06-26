import { useEffect, useRef, useState } from 'react'
import type {
  ProjectBuildConfig,
  ProjectRuntimeConfig,
  RemoteImConfig,
  VisualStudioInstallation
} from '../../electron/preload'
import ProjectBuildSettingsSection, {
  formatBuildConfigSaveError,
  normalizeBuildConfigForHost
} from './ProjectBuildSettingsSection.js'
import ProjectRuntimeSettingsSection, {
  formatRuntimeConfigSaveError,
  normalizeRuntimeConfigForHost
} from './ProjectRuntimeSettingsSection.js'
import { showToast } from './Toast.js'
import { HabitMonitorPanel } from '../habit/HabitMonitorDialog.js'
import RemoteImSettingsSection from '../remote-im/RemoteImSettingsSection.js'

export const DEFAULT_SCREENSHOT_SHORTCUT = 'CommandOrControl+Shift+A'

export const SCREENSHOT_SHORTCUT_PRESETS = [
  { value: 'CommandOrControl+Shift+A', label: 'Ctrl/Cmd + Shift + A' },
  { value: 'CommandOrControl+Shift+S', label: 'Ctrl/Cmd + Shift + S' },
  { value: 'CommandOrControl+Alt+A', label: 'Ctrl/Cmd + Alt + A' },
  { value: 'Alt+Shift+A', label: 'Alt + Shift + A' }
] as const

export interface ScreenshotShortcutState {
  screenshotShortcut: string
  customExpanded: boolean
}

export function isPresetScreenshotShortcut(shortcut: string): boolean {
  return SCREENSHOT_SHORTCUT_PRESETS.some((preset) => preset.value === shortcut)
}

export function createScreenshotShortcutState(shortcut: string): ScreenshotShortcutState {
  return {
    screenshotShortcut: shortcut,
    customExpanded: !isPresetScreenshotShortcut(shortcut)
  }
}

export function selectScreenshotShortcutPreset(shortcut: string): ScreenshotShortcutState {
  return {
    screenshotShortcut: shortcut,
    customExpanded: false
  }
}

export function openCustomScreenshotShortcut(shortcut: string): ScreenshotShortcutState {
  return {
    screenshotShortcut: shortcut,
    customExpanded: true
  }
}

export function restoreDefaultScreenshotShortcut(): ScreenshotShortcutState {
  return {
    screenshotShortcut: DEFAULT_SCREENSHOT_SHORTCUT,
    customExpanded: false
  }
}

export interface AiSettings {
  ai_cli: 'claude' | 'codex'
  command?: string
  args?: string[]
  env?: Record<string, string>
}

export interface AppSettings {
  screenshotShortcutEnabled: boolean
  screenshotShortcut: string
}

interface AppSettingsSaveResponse {
  ok: boolean
  value?: AppSettings
  error?: string
}

export type SettingsSectionKey = 'shortcut' | 'ai' | 'build' | 'runtime' | 'remote-im' | 'habit'

interface ProjectSettingsSaveResponse {
  ok: boolean
  repaired?: boolean
  error?: string
}

interface BuildConfigSaveResponse extends ProjectSettingsSaveResponse {
  details?: Array<{ path: string; message: string }>
}

interface RuntimeConfigSaveResponse extends ProjectSettingsSaveResponse {
  details?: Array<{ path: string; message: string }>
}

export interface AiSettingsDialogProps {
  projectId: string | null
  initial: AiSettings
  initialRepoView: AiSettings
  initialAppSettings: AppSettings
  initialBuildConfig: ProjectBuildConfig
  buildConfigReady: boolean
  initialRuntimeConfig: ProjectRuntimeConfig
  runtimeConfigReady: boolean
  runtimeConfigDisabled?: boolean
  initialRemoteImConfig: RemoteImConfig
  remoteImConfigReady: boolean
  visualStudioInstallations: VisualStudioInstallation[]
  visualStudioInstallationsLoading: boolean
  onRefreshVisualStudioInstallations: () => void
  initialSection?: SettingsSectionKey
  mainCliLabel: string
  onClose: () => void
  onSaved: (next: AiSettings) => void
  onSavedRepoView: (next: AiSettings) => void
  onSavedAppSettings: (next: AppSettings) => void
  onSavedBuildConfig: (next: ProjectBuildConfig) => void
  onSavedRuntimeConfig: (next: ProjectRuntimeConfig) => void
  onSavedRemoteImConfig: (next: RemoteImConfig) => void
}

export function resolveSavedAppSettings(
  requested: AppSettings,
  saved: AppSettings | undefined
): AppSettings {
  return saved ?? requested
}

export function deriveAppSettingsSaveOutcome(
  requested: AppSettings,
  response: AppSettingsSaveResponse
): { appSettings: AppSettings; error: string | null } {
  return {
    appSettings: resolveSavedAppSettings(requested, response.value),
    error: response.ok ? null : response.error ?? 'save app settings failed'
  }
}

export function shouldApplyIncomingAppSettings(
  lastSyncedExternal: AppSettings,
  incoming: AppSettings,
  saving: boolean
): boolean {
  if (saving) return false
  return (
    lastSyncedExternal.screenshotShortcutEnabled !== incoming.screenshotShortcutEnabled ||
    lastSyncedExternal.screenshotShortcut !== incoming.screenshotShortcut
  )
}

export function getProjectSettingsRepairToastMessage(
  mainResponse: ProjectSettingsSaveResponse,
  repoResponse: ProjectSettingsSaveResponse,
  buildResponse?: ProjectSettingsSaveResponse,
  runtimeResponse?: ProjectSettingsSaveResponse
): string | null {
  return mainResponse.repaired ||
    repoResponse.repaired ||
    buildResponse?.repaired ||
    runtimeResponse?.repaired
    ? '项目设置文件已自动修复并保存'
    : null
}

export interface SaveProjectScopedSettingsParams {
  projectId: string
  nextMain: AiSettings
  nextRepoView: AiSettings
  nextBuildConfig?: ProjectBuildConfig
  nextRuntimeConfig?: ProjectRuntimeConfig
  setAiSettings: (
    projectId: string,
    next: AiSettings
  ) => Promise<ProjectSettingsSaveResponse>
  setRepoViewAiSettings: (
    projectId: string,
    next: AiSettings
  ) => Promise<ProjectSettingsSaveResponse>
  setBuildConfig?: (
    projectId: string,
    next: ProjectBuildConfig
  ) => Promise<BuildConfigSaveResponse>
  setRuntimeConfig?: (
    projectId: string,
    next: ProjectRuntimeConfig
  ) => Promise<RuntimeConfigSaveResponse>
  onMainSaved: (next: AiSettings) => void
  onRepoViewSaved: (next: AiSettings) => void
  onBuildConfigSaved?: (next: ProjectBuildConfig) => void
  onRuntimeConfigSaved?: (next: ProjectRuntimeConfig) => void
}

export async function saveProjectScopedSettings(
  params: SaveProjectScopedSettingsParams
): Promise<string | null> {
  const mainRes = await params.setAiSettings(params.projectId, params.nextMain)
  if (!mainRes.ok) throw new Error(mainRes.error ?? 'save main settings failed')
  params.onMainSaved(params.nextMain)

  const repoRes = await params.setRepoViewAiSettings(params.projectId, params.nextRepoView)
  if (!repoRes.ok) throw new Error(repoRes.error ?? 'save repo-view settings failed')
  params.onRepoViewSaved(params.nextRepoView)

  let buildRes: ProjectSettingsSaveResponse | undefined
  if (params.nextBuildConfig && params.setBuildConfig && params.onBuildConfigSaved) {
    const nextBuildRes = await params.setBuildConfig(params.projectId, params.nextBuildConfig)
    if (!nextBuildRes.ok) {
      throw new Error(
        formatBuildConfigSaveError(
          nextBuildRes.error ?? 'save build config failed',
          nextBuildRes.details
        )
      )
    }
    params.onBuildConfigSaved(params.nextBuildConfig)
    buildRes = nextBuildRes
  }

  let runtimeRes: ProjectSettingsSaveResponse | undefined
  if (params.nextRuntimeConfig && params.setRuntimeConfig && params.onRuntimeConfigSaved) {
    const nextRuntimeRes = await params.setRuntimeConfig(params.projectId, params.nextRuntimeConfig)
    if (!nextRuntimeRes.ok) {
      throw new Error(
        formatRuntimeConfigSaveError(
          nextRuntimeRes.error ?? 'save runtime config failed',
          nextRuntimeRes.details
        )
      )
    }
    params.onRuntimeConfigSaved(params.nextRuntimeConfig)
    runtimeRes = nextRuntimeRes
  }

  return getProjectSettingsRepairToastMessage(mainRes, repoRes, buildRes, runtimeRes)
}

function toEnvText(env: Record<string, string> | undefined): string {
  return Object.entries(env ?? {})
    .map(([key, value]) => `${key}=${value}`)
    .join('\n')
}

function fromForm(
  aiCli: 'claude' | 'codex',
  command: string,
  argsText: string,
  envText: string
): AiSettings {
  return {
    ai_cli: aiCli,
    command: command.trim() || undefined,
    args: argsText.trim().length ? argsText.trim().split(/\s+/) : undefined,
    env: Object.fromEntries(
      envText
        .split('\n')
        .map((line) => line.trim())
        .filter((line) => line.includes('='))
        .map((line) => {
          const index = line.indexOf('=')
          return [line.slice(0, index).trim(), line.slice(index + 1).trim()]
        })
    )
  }
}

function SettingsSection(props: {
  title: string
  aiCli: 'claude' | 'codex'
  command: string
  argsText: string
  envText: string
  onAiCli: (next: 'claude' | 'codex') => void
  onCommand: (next: string) => void
  onArgs: (next: string) => void
  onEnv: (next: string) => void
}): JSX.Element {
  return (
    <section className="ai-settings-card ai-settings-ai-card">
      <div className="ai-settings-card-head">
        <span className="ai-settings-card-icon">AI</span>
        <div>
          <div className="ai-settings-title">{props.title}</div>
          <div className="ai-settings-card-subtitle">控制主终端实际启动的 AI CLI。</div>
        </div>
      </div>
      <div className="ai-settings-form-grid">
        <label>
          AI CLI
          <select
            value={props.aiCli}
            onChange={(event) => props.onAiCli(event.target.value as 'claude' | 'codex')}
          >
            <option value="claude">Claude Code (默认 acceptEdits)</option>
            <option value="codex">Codex (workspace-write + never)</option>
          </select>
        </label>
        <label>
          Binary override
          <input
            type="text"
            value={props.command}
            onChange={(event) => props.onCommand(event.target.value)}
            placeholder={props.aiCli === 'codex' ? 'codex' : 'claude'}
          />
        </label>
        <label>
          附加 args
          <input
            type="text"
            value={props.argsText}
            onChange={(event) => props.onArgs(event.target.value)}
            placeholder="--foo --bar"
          />
        </label>
        <label className="ai-settings-grid-full">
          环境变量
          <textarea
            value={props.envText}
            onChange={(event) => props.onEnv(event.target.value)}
            rows={4}
            placeholder="KEY=VALUE"
          />
        </label>
      </div>
    </section>
  )
}

function ScreenshotSettingsSection(props: {
  enabled: boolean
  shortcut: string
  customExpanded: boolean
  disabled: boolean
  onEnabledChange: (next: boolean) => void
  onPresetSelect: (next: string) => void
  onCustomOpen: () => void
  onShortcutChange: (next: string) => void
  onRestoreDefaults: () => void
}): JSX.Element {
  const shouldShowCustomInput = props.customExpanded || !isPresetScreenshotShortcut(props.shortcut)

  return (
    <section className="ai-settings-card ai-settings-hero-card">
      <div className="ai-settings-hero-main">
        <span className="ai-settings-hero-icon">＋</span>
        <div>
          <div className="ai-settings-hero-title">全局快捷键</div>
          <div className="ai-settings-hero-copy">截图采样入口，保存后立即生效。</div>
        </div>
      </div>
      <div className="ai-settings-hero-status">
        <label className="ai-settings-checkbox ai-settings-hero-toggle">
          <input
            type="checkbox"
            checked={props.enabled}
            onChange={(event) => props.onEnabledChange(event.target.checked)}
            disabled={props.disabled}
          />
          <span>{props.enabled ? '截图已启用' : '截图已关闭'}</span>
        </label>
        <span className="ai-settings-shortcut-current">{props.shortcut || DEFAULT_SCREENSHOT_SHORTCUT}</span>
        <button
          type="button"
          className="drawer-btn ai-settings-restore-btn"
          onClick={props.onRestoreDefaults}
          disabled={props.disabled}
        >
          恢复默认
        </button>
      </div>
      <div className="ai-settings-shortcut-card">
        <div>
          <div className="ai-settings-title">快捷键预设</div>
          <div className="ai-settings-card-subtitle">常用快捷键一键切换，自定义模式单独展开。</div>
        </div>
        <div className="ai-settings-shortcut-presets">
          {SCREENSHOT_SHORTCUT_PRESETS.map((preset) => (
            <button
              key={preset.value}
              type="button"
              className="drawer-btn ai-settings-shortcut-preset"
              aria-pressed={props.shortcut === preset.value}
              onClick={() => props.onPresetSelect(preset.value)}
              disabled={props.disabled}
            >
              {preset.label}
            </button>
          ))}
          <button
            type="button"
            className="drawer-btn ai-settings-shortcut-custom-toggle"
            aria-pressed={shouldShowCustomInput}
            onClick={props.onCustomOpen}
            disabled={props.disabled}
          >
            自定义
          </button>
        </div>
      </div>
      {shouldShowCustomInput && (
        <label className="ai-settings-shortcut-custom">
          快捷键
          <input
            type="text"
            value={props.shortcut}
            onChange={(event) => props.onShortcutChange(event.target.value)}
            placeholder={DEFAULT_SCREENSHOT_SHORTCUT}
            disabled={props.disabled}
          />
        </label>
      )}
      <div className="ai-settings-help">
        默认值：{DEFAULT_SCREENSHOT_SHORTCUT}。示例：Alt+Shift+S、CommandOrControl+Alt+X。
      </div>
    </section>
  )
}

export default function AiSettingsDialog(props: AiSettingsDialogProps): JSX.Element {
  const [aiCli, setAiCli] = useState<'claude' | 'codex'>(props.initial.ai_cli ?? 'claude')
  const [command, setCommand] = useState<string>(props.initial.command ?? '')
  const [argsText, setArgsText] = useState<string>((props.initial.args ?? []).join(' '))
  const [envText, setEnvText] = useState<string>(toEnvText(props.initial.env))

  const [repoAiCli, setRepoAiCli] = useState<'claude' | 'codex'>(
    props.initialRepoView.ai_cli ?? 'claude'
  )
  const [repoCommand, setRepoCommand] = useState<string>(props.initialRepoView.command ?? '')
  const [repoArgsText, setRepoArgsText] = useState<string>(
    (props.initialRepoView.args ?? []).join(' ')
  )
  const [repoEnvText, setRepoEnvText] = useState<string>(toEnvText(props.initialRepoView.env))

  const [screenshotShortcutEnabled, setScreenshotShortcutEnabled] = useState<boolean>(
    props.initialAppSettings.screenshotShortcutEnabled
  )
  const [screenshotShortcut, setScreenshotShortcut] = useState<string>(
    props.initialAppSettings.screenshotShortcut
  )
  const [screenshotShortcutCustomExpanded, setScreenshotShortcutCustomExpanded] = useState<boolean>(
    createScreenshotShortcutState(props.initialAppSettings.screenshotShortcut).customExpanded
  )
  const [buildConfig, setBuildConfig] = useState<ProjectBuildConfig>(props.initialBuildConfig)
  const [runtimeConfig, setRuntimeConfig] = useState<ProjectRuntimeConfig>(
    props.initialRuntimeConfig
  )
  const [remoteImConfig, setRemoteImConfig] = useState<RemoteImConfig>(
    props.initialRemoteImConfig
  )
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const lastSyncedAppSettingsRef = useRef<AppSettings>(props.initialAppSettings)
  const [activeSettingsSection, setActiveSettingsSection] = useState<SettingsSectionKey>(
    props.initialSection ?? 'shortcut'
  )
  const shortcutSectionRef = useRef<HTMLDivElement | null>(null)
  const aiSectionRef = useRef<HTMLDivElement | null>(null)
  const buildSectionRef = useRef<HTMLDivElement | null>(null)
  const runtimeSectionRef = useRef<HTMLDivElement | null>(null)
  const remoteImSectionRef = useRef<HTMLDivElement | null>(null)
  const habitSectionRef = useRef<HTMLDivElement | null>(null)

  const scrollToSettingsSection = (section: SettingsSectionKey): void => {
    setActiveSettingsSection(section)
    const target = {
      shortcut: shortcutSectionRef.current,
      ai: aiSectionRef.current,
      build: buildSectionRef.current,
      runtime: runtimeSectionRef.current,
      'remote-im': remoteImSectionRef.current,
      habit: habitSectionRef.current
    }[section]
    target?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  }

  useEffect(() => {
    const section = props.initialSection ?? 'shortcut'
    setActiveSettingsSection(section)
    window.setTimeout(() => {
      const target = {
        shortcut: shortcutSectionRef.current,
        ai: aiSectionRef.current,
        build: buildSectionRef.current,
        runtime: runtimeSectionRef.current,
        'remote-im': remoteImSectionRef.current,
        habit: habitSectionRef.current
      }[section]
      target?.scrollIntoView({ behavior: 'auto', block: 'start' })
    }, 0)
  }, [props.initialSection])

  useEffect(() => {
    if (
      !shouldApplyIncomingAppSettings(
        lastSyncedAppSettingsRef.current,
        props.initialAppSettings,
        saving
      )
    ) {
      return
    }
    setScreenshotShortcutEnabled(props.initialAppSettings.screenshotShortcutEnabled)
    setScreenshotShortcut(props.initialAppSettings.screenshotShortcut)
    setScreenshotShortcutCustomExpanded(
      createScreenshotShortcutState(props.initialAppSettings.screenshotShortcut).customExpanded
    )
    lastSyncedAppSettingsRef.current = props.initialAppSettings
  }, [props.initialAppSettings, saving])

  useEffect(() => {
    if (saving) return
    setAiCli(props.initial.ai_cli ?? 'claude')
    setCommand(props.initial.command ?? '')
    setArgsText((props.initial.args ?? []).join(' '))
    setEnvText(toEnvText(props.initial.env))
  }, [props.initial, saving])

  useEffect(() => {
    if (saving) return
    setRepoAiCli(props.initialRepoView.ai_cli ?? 'claude')
    setRepoCommand(props.initialRepoView.command ?? '')
    setRepoArgsText((props.initialRepoView.args ?? []).join(' '))
    setRepoEnvText(toEnvText(props.initialRepoView.env))
  }, [props.initialRepoView, saving])

  useEffect(() => {
    if (saving) return
    setBuildConfig(props.initialBuildConfig)
  }, [props.initialBuildConfig, saving])

  useEffect(() => {
    if (saving) return
    setRuntimeConfig(props.initialRuntimeConfig)
  }, [props.initialRuntimeConfig, saving])

  useEffect(() => {
    if (saving) return
    setRemoteImConfig(props.initialRemoteImConfig)
  }, [props.initialRemoteImConfig, saving])

  const handleRestoreDefaultShortcut = (): void => {
    const next = restoreDefaultScreenshotShortcut()
    setScreenshotShortcutEnabled(true)
    setScreenshotShortcut(next.screenshotShortcut)
    setScreenshotShortcutCustomExpanded(next.customExpanded)
  }

  const handlePresetSelect = (nextShortcut: string): void => {
    const next = selectScreenshotShortcutPreset(nextShortcut)
    setScreenshotShortcut(next.screenshotShortcut)
    setScreenshotShortcutCustomExpanded(next.customExpanded)
  }

  const handleCustomOpen = (): void => {
    const next = openCustomScreenshotShortcut(screenshotShortcut)
    setScreenshotShortcut(next.screenshotShortcut)
    setScreenshotShortcutCustomExpanded(next.customExpanded)
  }

  const handleSave = async (): Promise<void> => {
    const normalizedShortcut = screenshotShortcut.trim()
    if (screenshotShortcutEnabled && !normalizedShortcut) {
      setError('启用截图快捷键时，快捷键不能为空')
      return
    }

    setSaving(true)
    setError(null)

    const requestedAppSettings: AppSettings = {
      screenshotShortcutEnabled,
      screenshotShortcut: normalizedShortcut
    }
    const nextMain = fromForm(aiCli, command, argsText, envText)
    const nextRepoView = fromForm(repoAiCli, repoCommand, repoArgsText, repoEnvText)
    const nextBuildConfig = props.buildConfigReady
      ? normalizeBuildConfigForHost(buildConfig)
      : undefined
    const nextRuntimeConfig = props.runtimeConfigReady
      ? normalizeRuntimeConfigForHost(runtimeConfig)
      : undefined

    try {
      const appRes = await window.api.settings.setAppSettings(requestedAppSettings)
      const appOutcome = deriveAppSettingsSaveOutcome(requestedAppSettings, appRes)

      if (appRes.value || appRes.ok) {
        setScreenshotShortcutEnabled(appOutcome.appSettings.screenshotShortcutEnabled)
        setScreenshotShortcut(appOutcome.appSettings.screenshotShortcut)
        setScreenshotShortcutCustomExpanded(
          createScreenshotShortcutState(appOutcome.appSettings.screenshotShortcut).customExpanded
        )
        lastSyncedAppSettingsRef.current = appOutcome.appSettings
        props.onSavedAppSettings(appOutcome.appSettings)
      }

      if (appOutcome.error) {
        throw new Error(appOutcome.error)
      }

      if (props.projectId) {
        const repairToast = await saveProjectScopedSettings({
          projectId: props.projectId,
          nextMain,
          nextRepoView,
          nextBuildConfig,
          nextRuntimeConfig,
          setAiSettings: window.api.project.setAiSettings,
          setRepoViewAiSettings: window.api.project.setRepoViewAiSettings,
          setBuildConfig: props.buildConfigReady ? window.api.project.setBuildConfig : undefined,
          setRuntimeConfig: props.runtimeConfigReady ? window.api.project.setRuntimeConfig : undefined,
          onMainSaved: props.onSaved,
          onRepoViewSaved: props.onSavedRepoView,
          onBuildConfigSaved: props.buildConfigReady ? props.onSavedBuildConfig : undefined,
          onRuntimeConfigSaved: props.runtimeConfigReady ? props.onSavedRuntimeConfig : undefined
        })
        if (repairToast) {
          showToast(repairToast, { level: 'success' })
        }
        if (props.remoteImConfigReady) {
          const remoteImRes = await window.api.remoteIm.setConfig(props.projectId, remoteImConfig)
          if (!remoteImRes.ok) {
            throw new Error(remoteImRes.error ?? 'save remote IM config failed')
          }
          setRemoteImConfig(remoteImRes.value)
          props.onSavedRemoteImConfig(remoteImRes.value)
        }
      }

      props.onClose()
    } catch (cause: unknown) {
      setError(cause instanceof Error ? cause.message : String(cause))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div className="modal ai-settings-modal" onClick={(event) => event.stopPropagation()}>
        <header className="ai-settings-header">
          <div className="ai-settings-header-main">
            <span className="ai-settings-header-dot" />
            <div>
              <h3>设置中心</h3>
              <p>快捷键 · AI CLI · 项目构建 · 项目运行</p>
            </div>
          </div>
          <div className="ai-settings-header-actions">
            <span className="ai-settings-project-badge">
              <span className="ai-settings-project-badge-dot" />
              项目级保存
            </span>
            <button className="modal-close" onClick={props.onClose} aria-label="关闭">
              ×
            </button>
          </div>
        </header>

        <div className="ai-settings-shell">
          <aside className="ai-settings-sidebar" aria-label="设置分组">
            <div className="ai-settings-sidebar-label">CONFIG</div>
            <div className="ai-settings-nav-list">
              <button
                type="button"
                className={
                  activeSettingsSection === 'shortcut'
                    ? 'ai-settings-nav-item active'
                    : 'ai-settings-nav-item'
                }
                aria-controls="ai-settings-shortcut-section"
                aria-current={activeSettingsSection === 'shortcut' ? 'true' : undefined}
                onClick={() => scrollToSettingsSection('shortcut')}
              >
                <span className="ai-settings-nav-icon">＋</span>
                <span>
                  <strong>全局快捷键</strong>
                  <small>截图采样入口</small>
                </span>
              </button>
              <button
                type="button"
                className={
                  activeSettingsSection === 'ai'
                    ? 'ai-settings-nav-item active'
                    : 'ai-settings-nav-item'
                }
                aria-controls="ai-settings-ai-section"
                aria-current={activeSettingsSection === 'ai' ? 'true' : undefined}
                onClick={() => scrollToSettingsSection('ai')}
              >
                <span className="ai-settings-nav-icon">AI</span>
                <span>
                  <strong>主会话 AI</strong>
                  <small>Claude / Codex</small>
                </span>
              </button>
              <button
                type="button"
                className={
                  activeSettingsSection === 'build'
                    ? 'ai-settings-nav-item active'
                    : 'ai-settings-nav-item'
                }
                aria-controls="ai-settings-build-section"
                aria-current={activeSettingsSection === 'build' ? 'true' : undefined}
                onClick={() => scrollToSettingsSection('build')}
              >
                <span className="ai-settings-nav-icon">▤</span>
                <span>
                  <strong>项目构建</strong>
                  <small>步骤 / 环境 / 编码</small>
                </span>
              </button>
              <button
                type="button"
                className={
                  activeSettingsSection === 'runtime'
                    ? 'ai-settings-nav-item active'
                    : 'ai-settings-nav-item'
                }
                aria-controls="ai-settings-runtime-section"
                aria-current={activeSettingsSection === 'runtime' ? 'true' : undefined}
                onClick={() => scrollToSettingsSection('runtime')}
              >
                <span className="ai-settings-nav-icon">▶</span>
                <span>
                  <strong>项目运行</strong>
                  <small>启动命令</small>
                </span>
              </button>
              <button
                type="button"
                className={
                  activeSettingsSection === 'remote-im'
                    ? 'ai-settings-nav-item active'
                    : 'ai-settings-nav-item'
                }
                aria-controls="ai-settings-remote-im-section"
                aria-current={activeSettingsSection === 'remote-im' ? 'true' : undefined}
                onClick={() => scrollToSettingsSection('remote-im')}
              >
                <span className="ai-settings-nav-icon">IM</span>
                <span>
                  <strong>远程 IM</strong>
                  <small>手机消息接入 AICLI</small>
                </span>
              </button>
              <button
                type="button"
                className={
                  activeSettingsSection === 'habit'
                    ? 'ai-settings-nav-item ai-settings-habit-entry active'
                    : 'ai-settings-nav-item ai-settings-habit-entry'
                }
                aria-controls="ai-settings-habit-section"
                aria-current={activeSettingsSection === 'habit' ? 'true' : undefined}
                onClick={() => scrollToSettingsSection('habit')}
              >
                <span className="ai-settings-nav-icon">🧠</span>
                <span>
                  <strong>习惯监控</strong>
                  <small>查看活跃流程和采集设置</small>
                </span>
              </button>
            </div>
            <div className="ai-settings-current-project">
              <span>当前项目</span>
              <strong>{props.projectId ?? '未选择项目'}</strong>
              <small>{props.projectId ? '已选择' : '项目级配置不可编辑'}</small>
            </div>
          </aside>

          <main className="ai-settings-content">
            <div
              id="ai-settings-shortcut-section"
              ref={shortcutSectionRef}
              className="ai-settings-section-anchor"
            >
              <ScreenshotSettingsSection
                enabled={screenshotShortcutEnabled}
                shortcut={screenshotShortcut}
                customExpanded={screenshotShortcutCustomExpanded}
                disabled={saving}
                onEnabledChange={setScreenshotShortcutEnabled}
                onPresetSelect={handlePresetSelect}
                onCustomOpen={handleCustomOpen}
                onShortcutChange={setScreenshotShortcut}
                onRestoreDefaults={handleRestoreDefaultShortcut}
              />
            </div>

            <div className="ai-settings-content-grid">
              <div
                id="ai-settings-ai-section"
                ref={aiSectionRef}
                className="ai-settings-section-anchor"
              >
                {props.projectId ? (
                  <SettingsSection
                    title="主会话 AI"
                    aiCli={aiCli}
                    command={command}
                    argsText={argsText}
                    envText={envText}
                    onAiCli={setAiCli}
                    onCommand={setCommand}
                    onArgs={setArgsText}
                    onEnv={setEnvText}
                  />
                ) : (
                  <section className="ai-settings-card ai-settings-no-project-card">
                    <div className="ai-settings-card-head">
                      <span className="ai-settings-card-icon">AI</span>
                      <div>
                        <div className="ai-settings-title">AI CLI</div>
                        <div className="ai-settings-note">选择项目后可编辑 AI CLI 配置</div>
                      </div>
                    </div>
                  </section>
                )}
              </div>

              <div
                id="ai-settings-runtime-section"
                ref={runtimeSectionRef}
                className="ai-settings-panel ai-settings-runtime-panel ai-settings-section-anchor"
              >
                <ProjectRuntimeSettingsSection
                  projectId={props.projectId}
                  loading={props.projectId !== null && !props.runtimeConfigReady}
                  value={runtimeConfig}
                  disabled={saving || props.runtimeConfigDisabled === true}
                  visualStudioInstallations={props.visualStudioInstallations}
                  visualStudioInstallationsLoading={props.visualStudioInstallationsLoading}
                  onRefreshVisualStudioInstallations={props.onRefreshVisualStudioInstallations}
                  onChange={setRuntimeConfig}
                />
              </div>

              <div
                id="ai-settings-build-section"
                ref={buildSectionRef}
                className="ai-settings-panel ai-settings-build-panel ai-settings-grid-full ai-settings-section-anchor"
              >
                <ProjectBuildSettingsSection
                  projectId={props.projectId}
                  loading={props.projectId !== null && !props.buildConfigReady}
                  value={buildConfig}
                  disabled={saving}
                  visualStudioInstallations={props.visualStudioInstallations}
                  visualStudioInstallationsLoading={props.visualStudioInstallationsLoading}
                  onRefreshVisualStudioInstallations={props.onRefreshVisualStudioInstallations}
                  onChange={setBuildConfig}
                />
              </div>

              <div
                id="ai-settings-remote-im-section"
                ref={remoteImSectionRef}
                className="ai-settings-panel ai-settings-remote-im-panel ai-settings-grid-full ai-settings-section-anchor"
              >
                <RemoteImSettingsSection
                  config={remoteImConfig}
                  disabled={saving || props.projectId === null || !props.remoteImConfigReady}
                  onChange={setRemoteImConfig}
                />
              </div>

              <div
                id="ai-settings-habit-section"
                ref={habitSectionRef}
                className="ai-settings-panel ai-settings-habit-panel ai-settings-grid-full ai-settings-section-anchor"
              >
                <HabitMonitorPanel mainCliLabel={props.mainCliLabel} />
              </div>
            </div>
            {error && <div className="modal-error">⚠ {error}</div>}
          </main>
        </div>

        <footer className="ai-settings-footer">
          <span>变更点击“保存设置”后生效。</span>
          <div className="ai-settings-footer-actions">
            <button className="drawer-btn" onClick={props.onClose}>
              取消
            </button>
            <button className="drawer-btn primary" onClick={handleSave} disabled={saving}>
              {saving ? '保存中…' : '保存设置'}
            </button>
          </div>
        </footer>
      </div>
    </div>
  )
}
