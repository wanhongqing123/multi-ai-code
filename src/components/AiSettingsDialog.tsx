import { useState } from 'react'

export interface AiSettings {
  /** 'claude' | 'codex' */
  ai_cli: 'claude' | 'codex'
  /** Optional override of the CLI binary name (defaults to ai_cli). */
  command?: string
  /** Extra args appended to the default ones. */
  args?: string[]
  env?: Record<string, string>
}

export interface AiSettingsDialogProps {
  projectId: string
  initial: AiSettings
  initialRepoView: AiSettings
  onClose: () => void
  onSaved: (next: AiSettings) => void
  onSavedRepoView: (next: AiSettings) => void
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
        .map((l) => l.trim())
        .filter((l) => l.includes('='))
        .map((l) => {
          const idx = l.indexOf('=')
          return [l.slice(0, idx).trim(), l.slice(idx + 1).trim()]
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
          <option value="claude">Claude Code (默认)</option>
          <option value="codex">Codex (--full-auto)</option>
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

export default function AiSettingsDialog(
  props: AiSettingsDialogProps
): JSX.Element {
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

  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const handleSave = async (): Promise<void> => {
    setSaving(true)
    setError(null)
    const nextMain = fromForm(aiCli, command, argsText, envText)
    const nextRepoView = fromForm(repoAiCli, repoCommand, repoArgsText, repoEnvText)
    try {
      const [mainRes, repoRes] = await Promise.all([
        window.api.project.setAiSettings(props.projectId, nextMain),
        window.api.project.setRepoViewAiSettings(props.projectId, nextRepoView)
      ])
      if (!mainRes.ok) throw new Error(mainRes.error ?? 'save main settings failed')
      if (!repoRes.ok) throw new Error(repoRes.error ?? 'save repo-view settings failed')
      props.onSaved(nextMain)
      props.onSavedRepoView(nextRepoView)
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
          <h3>AI 设置</h3>
          <button className="modal-close" onClick={props.onClose} aria-label="关闭">
            ×
          </button>
        </div>
        <div className="ai-settings-body">
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
          <SettingsSection
            title="仓库查看分析 AI（默认 Claude）"
            aiCli={repoAiCli}
            command={repoCommand}
            argsText={repoArgsText}
            envText={repoEnvText}
            onAiCli={setRepoAiCli}
            onCommand={setRepoCommand}
            onArgs={setRepoArgsText}
            onEnv={setRepoEnvText}
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
