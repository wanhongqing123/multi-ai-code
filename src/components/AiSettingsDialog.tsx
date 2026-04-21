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
  onClose: () => void
  onSaved: (next: AiSettings) => void
}

export default function AiSettingsDialog(
  props: AiSettingsDialogProps
): JSX.Element {
  const [aiCli, setAiCli] = useState<'claude' | 'codex'>(
    props.initial.ai_cli ?? 'claude'
  )
  const [command, setCommand] = useState<string>(props.initial.command ?? '')
  const [argsText, setArgsText] = useState<string>(
    (props.initial.args ?? []).join(' ')
  )
  const [envText, setEnvText] = useState<string>(
    Object.entries(props.initial.env ?? {})
      .map(([k, v]) => `${k}=${v}`)
      .join('\n')
  )
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const handleSave = async (): Promise<void> => {
    setSaving(true)
    setError(null)
    const next: AiSettings = {
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
    try {
      const res = await window.api.project.setAiSettings(props.projectId, next)
      if (!res.ok) throw new Error(res.error ?? 'save failed')
      props.onSaved(next)
      props.onClose()
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div
        className="modal ai-settings-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-head">
          <h3>AI 设置</h3>
          <button
            className="modal-close"
            onClick={props.onClose}
            aria-label="关闭"
          >
            ×
          </button>
        </div>
        <div className="modal-body">
          <div className="modal-field">
            <label>AI CLI</label>
            <select
              value={aiCli}
              onChange={(e) =>
                setAiCli(e.target.value as 'claude' | 'codex')
              }
            >
              <option value="claude">Claude Code (默认)</option>
              <option value="codex">Codex (--full-auto)</option>
            </select>
          </div>
          <div className="modal-field">
            <label>Binary override (留空使用默认)</label>
            <input
              type="text"
              value={command}
              onChange={(e) => setCommand(e.target.value)}
              placeholder={aiCli === 'codex' ? 'codex' : 'claude'}
            />
          </div>
          <div className="modal-field">
            <label>附加 args (空格分隔)</label>
            <input
              type="text"
              value={argsText}
              onChange={(e) => setArgsText(e.target.value)}
              placeholder="--foo --bar"
            />
          </div>
          <div className="modal-field">
            <label>环境变量 (每行 KEY=VALUE)</label>
            <textarea
              value={envText}
              onChange={(e) => setEnvText(e.target.value)}
              rows={4}
            />
          </div>
          {error && <div className="modal-error">⚠ {error}</div>}
        </div>
        <div className="modal-actions">
          <button className="drawer-btn" onClick={props.onClose}>
            取消
          </button>
          <button
            className="drawer-btn primary"
            onClick={handleSave}
            disabled={saving}
          >
            {saving ? '保存中…' : '保存'}
          </button>
        </div>
      </div>
    </div>
  )
}
