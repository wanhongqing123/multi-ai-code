import { useEffect, useState } from 'react'
import type { AiSettings, AiPermissionMode, OpenCodeProviderProfile } from '../../electron/preload'
// 类型真源在 electron 侧，这里 re-export 兼容既有从本组件引用它们的地方（App.tsx / RepoViewerWindow）。
export type { AiSettings, AiPermissionMode, OpenCodeProviderProfile }
import { DEFAULT_AI_PERMISSION_MODE } from '../utils/cliLaunchArgs.js'
import { showToast } from './Toast.js'

export const DEFAULT_AI_CLI = 'codex' as const
export { DEFAULT_AI_PERMISSION_MODE }

const AI_CLI_OPTIONS = [
  {
    value: 'codex',
    label: 'Codex (推荐)'
  },
  {
    value: 'opencode',
    label: 'OpenCode'
  },
  {
    value: 'claude',
    label: 'Claude Code (不建议使用)'
  }
] as const

// 权限只给两档。背后是哪些 --dangerously-* 参数不暴露给用户，
// 想自己传参的走「高级参数设置」。
const PERMISSION_OPTIONS = [
  { value: 'full-access', label: '完全访问权限' },
  { value: 'default', label: '默认权限' }
] as const satisfies readonly { value: AiPermissionMode; label: string }[]

type AiCliKind = typeof AI_CLI_OPTIONS[number]['value']

interface ProjectSettingsSaveResponse {
  ok: boolean
  repaired?: boolean
  error?: string
}

export interface AiSettingsDialogProps {
  projectId: string | null
  initial: AiSettings
  onClose: () => void
  onSaved: (next: AiSettings) => void
}

export function getProjectSettingsRepairToastMessage(
  mainResponse: ProjectSettingsSaveResponse
): string | null {
  return mainResponse.repaired ? '项目设置文件已自动修复并保存' : null
}

export interface SaveProjectScopedSettingsParams {
  projectId: string
  nextMain: AiSettings
  setAiSettings: (projectId: string, next: AiSettings) => Promise<ProjectSettingsSaveResponse>
  onMainSaved: (next: AiSettings) => void
}

export async function saveProjectScopedSettings(
  params: SaveProjectScopedSettingsParams
): Promise<string | null> {
  const mainRes = await params.setAiSettings(params.projectId, params.nextMain)
  if (!mainRes.ok) throw new Error(mainRes.error ?? 'save main settings failed')
  params.onMainSaved(params.nextMain)
  return getProjectSettingsRepairToastMessage(mainRes)
}

function toEnvText(env: Record<string, string> | undefined): string {
  return Object.entries(env ?? {})
    .map(([key, value]) => `${key}=${value}`)
    .join('\n')
}

function fromForm(
  aiCli: AiCliKind,
  permissionMode: AiPermissionMode,
  argsText: string,
  envText: string
): AiSettings {
  return {
    ai_cli: aiCli,
    permission_mode: permissionMode,
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
  aiCli: AiCliKind
  permissionMode: AiPermissionMode
  advancedOpen: boolean
  argsText: string
  envText: string
  onAiCli: (next: AiCliKind) => void
  onPermissionMode: (next: AiPermissionMode) => void
  onAdvancedOpen: (next: boolean) => void
  onArgs: (next: string) => void
  onEnv: (next: string) => void
}): JSX.Element {
  return (
    <section className="ai-settings-card ai-settings-ai-card">
      <div className="ai-settings-form-grid">
        <label>
          AI CLI
          <select
            value={props.aiCli}
            onChange={(event) => props.onAiCli(event.target.value as AiCliKind)}
          >
            {AI_CLI_OPTIONS.map((option) => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
        </label>
        <label>
          权限
          <select
            value={props.permissionMode}
            onChange={(event) => props.onPermissionMode(event.target.value as AiPermissionMode)}
          >
            {PERMISSION_OPTIONS.map((option) => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
        </label>
        {/* 启动参数不该是常规操作：默认折叠，需要按特定参数启动的人自己展开。 */}
        <div className="ai-settings-grid-full ai-settings-advanced">
          <button
            type="button"
            className="ai-settings-advanced-toggle"
            aria-expanded={props.advancedOpen}
            aria-controls="ai-settings-advanced-body"
            onClick={() => props.onAdvancedOpen(!props.advancedOpen)}
          >
            <span className="ai-settings-advanced-caret" aria-hidden="true">
              {props.advancedOpen ? '▾' : '▸'}
            </span>
            高级参数设置
          </button>
          {props.advancedOpen && (
            <div id="ai-settings-advanced-body" className="ai-settings-form-grid">
              <label className="ai-settings-grid-full">
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
          )}
        </div>
      </div>
    </section>
  )
}

export default function AiSettingsDialog(props: AiSettingsDialogProps): JSX.Element {
  const [aiCli, setAiCli] = useState<AiCliKind>(props.initial.ai_cli ?? DEFAULT_AI_CLI)
  const [permissionMode, setPermissionMode] = useState<AiPermissionMode>(
    props.initial.permission_mode ?? DEFAULT_AI_PERMISSION_MODE
  )
  const [argsText, setArgsText] = useState<string>((props.initial.args ?? []).join(' '))
  const [envText, setEnvText] = useState<string>(toEnvText(props.initial.env))
  // 已经填过 args/env 的项目，打开设置就该看见它们，否则会以为参数丢了。
  const [advancedOpen, setAdvancedOpen] = useState<boolean>(
    (props.initial.args ?? []).length > 0 || Object.keys(props.initial.env ?? {}).length > 0
  )
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (saving) return
    setAiCli(props.initial.ai_cli ?? DEFAULT_AI_CLI)
    setPermissionMode(props.initial.permission_mode ?? DEFAULT_AI_PERMISSION_MODE)
    setArgsText((props.initial.args ?? []).join(' '))
    setEnvText(toEnvText(props.initial.env))
  }, [props.initial, saving])

  const handleSave = async (): Promise<void> => {
    setSaving(true)
    setError(null)

    const nextMain = fromForm(aiCli, permissionMode, argsText, envText)

    try {
      if (props.projectId) {
        const repairToast = await saveProjectScopedSettings({
          projectId: props.projectId,
          nextMain,
          setAiSettings: window.api.project.setAiSettings,
          onMainSaved: props.onSaved
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
        <header className="ai-settings-header">
          <div className="ai-settings-header-main">
            <h3>设置</h3>
          </div>
          <div className="ai-settings-header-actions">
            <button className="modal-close" onClick={props.onClose} aria-label="关闭">
              ×
            </button>
          </div>
        </header>

        <div className="ai-settings-shell">
          <main className="ai-settings-content">
            <div id="ai-settings-ai-section" className="ai-settings-section-anchor">
              {props.projectId ? (
                <SettingsSection
                  aiCli={aiCli}
                  permissionMode={permissionMode}
                  advancedOpen={advancedOpen}
                  argsText={argsText}
                  envText={envText}
                  onAiCli={setAiCli}
                  onPermissionMode={setPermissionMode}
                  onAdvancedOpen={setAdvancedOpen}
                  onArgs={setArgsText}
                  onEnv={setEnvText}
                />
              ) : (
                <section className="ai-settings-card ai-settings-no-project-card">
                  <div className="ai-settings-note">选择工作空间后可编辑 AI CLI 配置</div>
                </section>
              )}
            </div>
            {error && <div className="modal-error">⚠ {error}</div>}
          </main>
        </div>

        <footer className="ai-settings-footer">
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
