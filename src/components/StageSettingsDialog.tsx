import { useEffect, useState } from 'react'

const STAGE_NAMES: Record<number, string> = {
  1: '方案设计',
  2: '方案实施',
  3: '方案验收',
  4: '测试验证'
}

const DEFAULTS: Record<string, { command: string; args: string }> = {
  '1': { command: 'codex', args: '--full-auto' },
  '2': { command: 'claude', args: '--permission-mode auto' },
  '3': { command: 'claude', args: '--permission-mode auto' },
  '4': { command: 'claude', args: '--permission-mode auto' }
}

interface StageForm {
  command: string
  args: string
  envText: string
  skip: boolean
}

export interface StageSettingsDialogProps {
  projectId: string
  onClose: () => void
}

export default function StageSettingsDialog({ projectId, onClose }: StageSettingsDialogProps) {
  const [forms, setForms] = useState<Record<string, StageForm>>({})
  const [saving, setSaving] = useState(false)

  useEffect(() => {
    void (async () => {
      const cfg = await window.api.project.getStageConfigs(projectId)
      const next: Record<string, StageForm> = {}
      for (const s of ['1', '2', '3', '4']) {
        const c = (cfg[s] ?? {}) as any
        next[s] = {
          command: c.command ?? DEFAULTS[s].command,
          args: (c.args ?? DEFAULTS[s].args.split(' ')).join(' '),
          envText: c.env
            ? Object.entries(c.env).map(([k, v]) => `${k}=${v}`).join('\n')
            : '',
          skip: !!c.skip
        }
      }
      setForms(next)
    })()
  }, [projectId])

  async function save() {
    setSaving(true)
    try {
      const configs: Record<string, { command: string; args: string[]; env?: Record<string, string>; skip?: boolean }> = {}
      for (const s of ['1', '2', '3', '4']) {
        const f = forms[s]
        if (!f) continue
        const env: Record<string, string> = {}
        for (const line of f.envText.split(/\r?\n/)) {
          const m = line.match(/^\s*([^=\s]+)\s*=\s*(.*)$/)
          if (m) env[m[1]] = m[2].trim()
        }
        configs[s] = {
          command: f.command.trim() || DEFAULTS[s].command,
          args: f.args.split(/\s+/).filter(Boolean),
          ...(Object.keys(env).length ? { env } : {}),
          ...(f.skip ? { skip: true } : {})
        }
      }
      await window.api.project.setStageConfigs(projectId, configs)
      onClose()
    } finally {
      setSaving(false)
    }
  }

  function resetStage(s: string) {
    setForms((prev) => ({
      ...prev,
      [s]: { command: DEFAULTS[s].command, args: DEFAULTS[s].args, envText: '', skip: false }
    }))
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal stage-settings-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>⚙️ 阶段 CLI 配置</h3>
          <button className="modal-close" onClick={onClose}>×</button>
        </div>
        <div className="stage-settings-body">
          {['1', '2', '3', '4'].map((s) => {
            const f = forms[s]
            if (!f) return null
            return (
              <div key={s} className="stage-settings-card">
                <div className="stage-settings-title">
                  Stage {s} · {STAGE_NAMES[Number(s)]}
                  <label style={{ marginLeft: 'auto', marginRight: 8, fontSize: 11 }}>
                    <input
                      type="checkbox"
                      checked={f.skip}
                      onChange={(e) =>
                        setForms((p) => ({ ...p, [s]: { ...f, skip: e.target.checked } }))
                      }
                    />{' '}
                    跳过此阶段
                  </label>
                  <button className="tile-btn" onClick={() => resetStage(s)}>
                    恢复默认
                  </button>
                </div>
                <label>
                  CLI 命令
                  <input
                    className="plan-name-input"
                    value={f.command}
                    onChange={(e) =>
                      setForms((p) => ({ ...p, [s]: { ...f, command: e.target.value } }))
                    }
                  />
                </label>
                <label>
                  命令行参数（空格分隔）
                  <input
                    className="plan-name-input"
                    value={f.args}
                    onChange={(e) =>
                      setForms((p) => ({ ...p, [s]: { ...f, args: e.target.value } }))
                    }
                  />
                </label>
                <label>
                  环境变量（每行 KEY=VALUE）
                  <textarea
                    className="plan-name-input"
                    rows={3}
                    value={f.envText}
                    onChange={(e) =>
                      setForms((p) => ({ ...p, [s]: { ...f, envText: e.target.value } }))
                    }
                  />
                </label>
              </div>
            )
          })}
        </div>
        <div className="drawer-actions">
          <button className="drawer-btn" onClick={onClose}>取消</button>
          <button className="drawer-btn primary" onClick={save} disabled={saving}>
            {saving ? '保存中…' : '保存'}
          </button>
        </div>
      </div>
    </div>
  )
}
