import type { RemoteImConfig } from '../../electron/preload.js'

export interface RemoteImSettingsSectionProps {
  config: RemoteImConfig
  disabled: boolean
  onChange: (next: RemoteImConfig) => void
}

export default function RemoteImSettingsSection(props: RemoteImSettingsSectionProps): JSX.Element {
  const { config, disabled, onChange } = props
  return (
    <section className="ai-settings-card remote-im-settings-section">
      <div className="ai-settings-title">远程 IM</div>
      <div className="ai-settings-note">
        这里仅配置当前项目是否接收远程 IM 任务；账号、凭证和联系人请在远程 IM 面板登录后管理。
      </div>

      <label className="ai-settings-checkbox">
        <input
          type="checkbox"
          disabled={disabled}
          checked={config.enabled}
          onChange={(event) => onChange({ ...config, enabled: event.currentTarget.checked })}
        />
        启用远程 IM
      </label>

      <div className="remote-im-settings-grid">
        <label>
          输出刷新间隔(ms)
          <input
            disabled={disabled}
            value={config.outputFlushIntervalMs}
            onChange={(event) =>
              onChange({
                ...config,
                outputFlushIntervalMs: Number(event.currentTarget.value)
              })
            }
            placeholder="2000"
          />
        </label>
        <label>
          单次回传字符数
          <input
            disabled={disabled}
            value={config.outputMaxChunkChars}
            onChange={(event) =>
              onChange({
                ...config,
                outputMaxChunkChars: Number(event.currentTarget.value)
              })
            }
            placeholder="1200"
          />
        </label>
      </div>
    </section>
  )
}
