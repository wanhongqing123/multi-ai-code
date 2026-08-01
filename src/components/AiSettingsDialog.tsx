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

export const DEFAULT_OPENCODE_PROVIDER_PROFILE: OpenCodeProviderProfile = {
  providerId: 'idealab',
  name: 'Alibaba ideaLAB',
  baseURL: 'https://idealab.alibaba-inc.com/api/openai/v1',
  mainModel: 'Qwen3.7-Max-DogFooding'
}

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

interface OpenCodeProviderForm {
  providerId: string
  name: string
  baseURL: string
  apiKey: string
  mainModel: string
  smallModel: string
}

function hasOpenCodeProviderProfile(profile: OpenCodeProviderProfile | undefined): boolean {
  return Boolean(
    profile &&
      [
        profile.providerId,
        profile.name,
        profile.baseURL,
        profile.apiKey,
        profile.mainModel,
        profile.smallModel
      ].some((value) => value?.trim())
  )
}

function toOpenCodeProviderForm(profile: OpenCodeProviderProfile | undefined): OpenCodeProviderForm {
  const resolvedProfile = hasOpenCodeProviderProfile(profile)
    ? profile
    : DEFAULT_OPENCODE_PROVIDER_PROFILE
  return {
    providerId: resolvedProfile?.providerId ?? '',
    name: resolvedProfile?.name ?? '',
    baseURL: resolvedProfile?.baseURL ?? '',
    apiKey: resolvedProfile?.apiKey ?? '',
    mainModel: resolvedProfile?.mainModel ?? '',
    smallModel: resolvedProfile?.smallModel ?? ''
  }
}

function fromOpenCodeProviderForm(form: OpenCodeProviderForm): OpenCodeProviderProfile | undefined {
  const providerId = form.providerId.trim()
  const baseURL = form.baseURL.trim()
  const mainModel = form.mainModel.trim()
  if (!providerId && !baseURL && !mainModel) return undefined
  return {
    providerId: providerId || undefined,
    name: form.name.trim() || undefined,
    baseURL: baseURL || undefined,
    apiKey: form.apiKey.trim() || undefined,
    mainModel: mainModel || undefined,
    smallModel: form.smallModel.trim() || undefined
  }
}

function fromForm(
  aiCli: AiCliKind,
  permissionMode: AiPermissionMode,
  argsText: string,
  envText: string,
  openCodeForm?: OpenCodeProviderForm
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
    ),
    opencode:
      aiCli === 'opencode' && openCodeForm
        ? fromOpenCodeProviderForm(openCodeForm)
        : undefined
  }
}

function SettingsSection(props: {
  aiCli: AiCliKind
  permissionMode: AiPermissionMode
  advancedOpen: boolean
  argsText: string
  envText: string
  openCodeForm: OpenCodeProviderForm
  onAiCli: (next: AiCliKind) => void
  onPermissionMode: (next: AiPermissionMode) => void
  onAdvancedOpen: (next: boolean) => void
  onArgs: (next: string) => void
  onEnv: (next: string) => void
  onOpenCodeForm: (next: OpenCodeProviderForm) => void
}): JSX.Element {
  const updateOpenCodeForm = (patch: Partial<OpenCodeProviderForm>): void => {
    props.onOpenCodeForm({ ...props.openCodeForm, ...patch })
  }

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
        {props.aiCli === 'opencode' ? (
          <div className="ai-settings-grid-full ai-settings-opencode-panel">
            <div className="ai-settings-title-row">
              <div>
                <div className="ai-settings-title">OpenCode 模型服务</div>
                <div className="ai-settings-card-subtitle">
                  自定义 OpenAI 兼容服务地址，启动时按当前进程注入。
                </div>
              </div>
            </div>
            <div className="ai-settings-form-grid">
              <label>
                服务名称
                <input
                  type="text"
                  value={props.openCodeForm.name}
                  onChange={(event) => updateOpenCodeForm({ name: event.target.value })}
                  placeholder="公司内网 DeepSeek"
                />
              </label>
              <label>
                Provider ID
                <input
                  type="text"
                  value={props.openCodeForm.providerId}
                  onChange={(event) => updateOpenCodeForm({ providerId: event.target.value })}
                  placeholder="multi-ai-deepseek-internal"
                />
              </label>
              <label className="ai-settings-grid-full">
                Base URL
                <input
                  type="text"
                  value={props.openCodeForm.baseURL}
                  onChange={(event) => updateOpenCodeForm({ baseURL: event.target.value })}
                  placeholder="https://your.gateway.example/v1"
                />
              </label>
              <label>
                API Key
                <input
                  type="password"
                  value={props.openCodeForm.apiKey}
                  onChange={(event) => updateOpenCodeForm({ apiKey: event.target.value })}
                  placeholder="sk-..."
                />
              </label>
              <label>
                主模型
                <input
                  type="text"
                  value={props.openCodeForm.mainModel}
                  onChange={(event) => updateOpenCodeForm({ mainModel: event.target.value })}
                  placeholder="deepseek-v4-pro"
                />
              </label>
              <label>
                小模型
                <input
                  type="text"
                  value={props.openCodeForm.smallModel}
                  onChange={(event) => updateOpenCodeForm({ smallModel: event.target.value })}
                  placeholder="默认同主模型"
                />
              </label>
            </div>
            <div className="ai-settings-help">
              API Key 会随当前项目配置保存，只用于启动 OpenCode 时注入当前进程。APIKEY
              的获取请参考：
              <a
                href="https://aistudio.alibaba-inc.com/#/aistudio/manage/accountManage"
                target="_blank"
                rel="noreferrer"
              >
                ideaLAB 账号管理
              </a>
              。
            </div>
          </div>
        ) : null}
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
  const [openCodeForm, setOpenCodeForm] = useState<OpenCodeProviderForm>(
    toOpenCodeProviderForm(props.initial.opencode)
  )
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
    setOpenCodeForm(toOpenCodeProviderForm(props.initial.opencode))
  }, [props.initial, saving])

  const handleSave = async (): Promise<void> => {
    setSaving(true)
    setError(null)

    const nextMain = fromForm(aiCli, permissionMode, argsText, envText, openCodeForm)

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
                  openCodeForm={openCodeForm}
                  onAiCli={setAiCli}
                  onPermissionMode={setPermissionMode}
                  onAdvancedOpen={setAdvancedOpen}
                  onArgs={setArgsText}
                  onEnv={setEnvText}
                  onOpenCodeForm={setOpenCodeForm}
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
