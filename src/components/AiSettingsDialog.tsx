import { useEffect, useRef, useState } from 'react'
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
  /** 'claude' | 'codex' */
  ai_cli: 'claude' | 'codex'
  /** Optional override of the CLI binary name (defaults to ai_cli). */
  command?: string
  /** Extra args appended to the default ones. */
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

export interface AiSettingsDialogProps {
  projectId: string | null
  initial: AiSettings
  initialRepoView: AiSettings
  initialAppSettings: AppSettings
  onClose: () => void
  onSaved: (next: AiSettings) => void
  onSavedRepoView: (next: AiSettings) => void
  onSavedAppSettings: (next: AppSettings) => void
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
  repoResponse: ProjectSettingsSaveResponse
): string | null {
  return mainResponse.repaired || repoResponse.repaired ? '项目设置文件已自动修复并保存' : null
}

function toEnvText(env: Record<string, string> | undefined): string {
  return Object.entries(env ?? {})
    .map(([k, v]) => `${k}=${v}`)
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
          const idx = line.indexOf('=')
          return [line.slice(0, idx).trim(), line.slice(idx + 1).trim()]
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
          onChange={(e) => props.onAiCli(e.target.value as 'claude' | 'codex')}
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
          onChange={(e) => props.onCommand(e.target.value)}
          placeholder={props.aiCli === 'codex' ? 'codex' : 'claude'}
        />
      </label>
      <label>
        附加 args (空格分隔)
        <input
          type="text"
          value={props.argsText}
          onChange={(e) => props.onArgs(e.target.value)}
          placeholder="--foo --bar"
        />
      </label>
      <label>
        环境变量 (每行 KEY=VALUE)
        <textarea
          value={props.envText}
          onChange={(e) => props.onEnv(e.target.value)}
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
          onChange={(e) => props.onEnabledChange(e.target.checked)}
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
            onChange={(e) => props.onShortcutChange(e.target.value)}
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

    try {
      const appRes = await window.api.settings.setAppSettings(requestedAppSettings)
      const appOutcome = deriveAppSettingsSaveOutcome(requestedAppSettings, appRes)

      if (appRes.value) {
        setScreenshotShortcutEnabled(appOutcome.appSettings.screenshotShortcutEnabled)
        setScreenshotShortcut(appOutcome.appSettings.screenshotShortcut)
        setScreenshotShortcutCustomExpanded(
          createScreenshotShortcutState(appOutcome.appSettings.screenshotShortcut).customExpanded
        )
        lastSyncedAppSettingsRef.current = appOutcome.appSettings
        props.onSavedAppSettings(appOutcome.appSettings)
      } else if (appRes.ok) {
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
        // Both endpoints persist into the same project.json file, so save them
        // sequentially to avoid overlapping writes that can re-corrupt metadata.
        const mainRes = await window.api.project.setAiSettings(props.projectId, nextMain)
        if (!mainRes.ok) throw new Error(mainRes.error ?? 'save main settings failed')
        const repoRes = await window.api.project.setRepoViewAiSettings(props.projectId, nextRepoView)
        if (!repoRes.ok) throw new Error(repoRes.error ?? 'save repo-view settings failed')
        const repairToast = getProjectSettingsRepairToastMessage(mainRes, repoRes)
        if (repairToast) {
          showToast(repairToast, { level: 'success' })
        }
        props.onSaved(nextMain)
        props.onSavedRepoView(nextRepoView)
      }

      props.onClose()
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div className="modal ai-settings-modal" onClick={(e) => e.stopPropagation()}>
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
          {/* 仓库查看分析 AI 配置已随仓库查看功能一并暂时隐藏。底层保存逻辑保留，
              只在 UI 中隐藏入口；将来恢复时把这块 SettingsSection 重新放回即可。 */}
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
