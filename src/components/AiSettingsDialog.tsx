import { useEffect, useRef, useState } from 'react'
import type {
  ProjectBuildConfig,
  ProjectRuntimeConfig,
  VisualStudioInstallation
} from '../../electron/preload'
import ProjectBuildSettingsSection, {
  formatBuildConfigSaveError
} from './ProjectBuildSettingsSection.js'
import ProjectRuntimeSettingsSection, {
  formatRuntimeConfigSaveError
} from './ProjectRuntimeSettingsSection.js'
import { showToast } from './Toast.js'

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
  visualStudioInstallations: VisualStudioInstallation[]
  visualStudioInstallationsLoading: boolean
  onRefreshVisualStudioInstallations: () => void
  onClose: () => void
  onSaved: (next: AiSettings) => void
  onSavedRepoView: (next: AiSettings) => void
  onSavedAppSettings: (next: AppSettings) => void
  onSavedBuildConfig: (next: ProjectBuildConfig) => void
  onSavedRuntimeConfig: (next: ProjectRuntimeConfig) => void
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
    <section className="ai-settings-card">
      <div className="ai-settings-title">{props.title}</div>
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
        Binary override (留空使用默认)
        <input
          type="text"
          value={props.command}
          onChange={(event) => props.onCommand(event.target.value)}
          placeholder={props.aiCli === 'codex' ? 'codex' : 'claude'}
        />
      </label>
      <label>
        附加 args (空格分隔)
        <input
          type="text"
          value={props.argsText}
          onChange={(event) => props.onArgs(event.target.value)}
          placeholder="--foo --bar"
        />
      </label>
      <label>
        环境变量 (每行 KEY=VALUE)
        <textarea
          value={props.envText}
          onChange={(event) => props.onEnv(event.target.value)}
          rows={4}
        />
      </label>
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
    <section className="ai-settings-card">
      <div className="ai-settings-title-row">
        <div className="ai-settings-title">截图快捷键（全局）</div>
        <button
          type="button"
          className="drawer-btn"
          onClick={props.onRestoreDefaults}
          disabled={props.disabled}
        >
          恢复默认
        </button>
      </div>
      <label className="ai-settings-checkbox">
        <input
          type="checkbox"
          checked={props.enabled}
          onChange={(event) => props.onEnabledChange(event.target.checked)}
          disabled={props.disabled}
        />
        <span>启用全局截图快捷键</span>
      </label>
      <div className="ai-settings-shortcut-presets">
        {SCREENSHOT_SHORTCUT_PRESETS.map((preset) => (
          <button
            key={preset.value}
            type="button"
            className="drawer-btn"
            aria-pressed={props.shortcut === preset.value}
            onClick={() => props.onPresetSelect(preset.value)}
            disabled={props.disabled}
          >
            {preset.label}
          </button>
        ))}
        <button
          type="button"
          className="drawer-btn"
          aria-pressed={shouldShowCustomInput}
          onClick={props.onCustomOpen}
          disabled={props.disabled}
        >
          自定义
        </button>
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
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const lastSyncedAppSettingsRef = useRef<AppSettings>(props.initialAppSettings)

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
    const nextBuildConfig = props.buildConfigReady ? buildConfig : undefined
    const nextRuntimeConfig = props.runtimeConfigReady ? runtimeConfig : undefined

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
        <div className="modal-head">
          <h3>设置</h3>
          <button className="modal-close" onClick={props.onClose} aria-label="关闭">
            ×
          </button>
        </div>
        <div className="ai-settings-body">
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
            <section className="ai-settings-card">
              <div className="ai-settings-title">AI CLI</div>
              <div className="ai-settings-note">选择项目后可编辑 AI CLI 配置</div>
            </section>
          )}
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
          {error && <div className="modal-error">⚠ {error}</div>}
        </div>
        <div className="modal-actions">
          <button className="drawer-btn" onClick={props.onClose}>
            取消
          </button>
          <button className="drawer-btn primary" onClick={handleSave} disabled={saving}>
            {saving ? '保存中…' : '保存'}
          </button>
        </div>
      </div>
    </div>
  )
}
