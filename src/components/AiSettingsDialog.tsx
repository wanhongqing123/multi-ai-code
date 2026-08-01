import { useEffect, useState } from 'react'
import type { AiSettings, OpenCodeProviderProfile } from '../../electron/preload'
// 类型真源在 electron 侧，这里 re-export 兼容既有从本组件引用它们的地方（App.tsx / RepoViewerWindow）。
export type { AiSettings, OpenCodeProviderProfile }
import { showToast } from './Toast.js'

export const DEFAULT_AI_CLI = 'codex' as const

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
  command: string,
  argsText: string,
  envText: string,
  openCodeForm?: OpenCodeProviderForm
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
    ),
    opencode:
      aiCli === 'opencode' && openCodeForm
        ? fromOpenCodeProviderForm(openCodeForm)
        : undefined
  }
}

function SettingsSection(props: {
  title: string
  aiCli: AiCliKind
  command: string
  argsText: string
  envText: string
  openCodeForm: OpenCodeProviderForm
  onAiCli: (next: AiCliKind) => void
  onCommand: (next: string) => void
  onArgs: (next: string) => void
  onEnv: (next: string) => void
  onOpenCodeForm: (next: OpenCodeProviderForm) => void
}): JSX.Element {
  const updateOpenCodeForm = (patch: Partial<OpenCodeProviderForm>): void => {
    props.onOpenCodeForm({ ...props.openCodeForm, ...patch })
  }

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
          Binary override
          <input
            type="text"
            value={props.command}
            onChange={(event) => props.onCommand(event.target.value)}
            placeholder={props.aiCli}
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
      </div>
    </section>
  )
}

export default function AiSettingsDialog(props: AiSettingsDialogProps): JSX.Element {
  const [aiCli, setAiCli] = useState<AiCliKind>(props.initial.ai_cli ?? DEFAULT_AI_CLI)
  const [command, setCommand] = useState<string>(props.initial.command ?? '')
  const [argsText, setArgsText] = useState<string>((props.initial.args ?? []).join(' '))
  const [envText, setEnvText] = useState<string>(toEnvText(props.initial.env))
  const [openCodeForm, setOpenCodeForm] = useState<OpenCodeProviderForm>(
    toOpenCodeProviderForm(props.initial.opencode)
  )
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (saving) return
    setAiCli(props.initial.ai_cli ?? DEFAULT_AI_CLI)
    setCommand(props.initial.command ?? '')
    setArgsText((props.initial.args ?? []).join(' '))
    setEnvText(toEnvText(props.initial.env))
    setOpenCodeForm(toOpenCodeProviderForm(props.initial.opencode))
  }, [props.initial, saving])

  const handleSave = async (): Promise<void> => {
    setSaving(true)
    setError(null)

    const nextMain = fromForm(aiCli, command, argsText, envText, openCodeForm)

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
            <span className="ai-settings-header-dot" />
            <div>
              <h3>设置中心</h3>
              <p>AI CLI</p>
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
          <main className="ai-settings-content">
            <div id="ai-settings-ai-section" className="ai-settings-section-anchor">
              {props.projectId ? (
                <SettingsSection
                  title="主会话 AI"
                  aiCli={aiCli}
                  command={command}
                  argsText={argsText}
                  envText={envText}
                  openCodeForm={openCodeForm}
                  onAiCli={setAiCli}
                  onCommand={setCommand}
                  onArgs={setArgsText}
                  onEnv={setEnvText}
                  onOpenCodeForm={setOpenCodeForm}
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
