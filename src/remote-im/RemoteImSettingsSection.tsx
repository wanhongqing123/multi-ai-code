import type { RemoteImConfig } from '../../electron/preload.js'

export interface RemoteImSettingsSectionProps {
  config: RemoteImConfig
  disabled: boolean
  onChange: (next: RemoteImConfig) => void
}

export default function RemoteImSettingsSection(props: RemoteImSettingsSectionProps): JSX.Element {
  const { config } = props
  return (
    <section className="ai-settings-card remote-im-settings-section">
      <div className="ai-settings-title">远程 IM</div>
      <div className="ai-settings-note">
        远程 IM 账号、SDKAppID、SecretKey 和连接动作由登录入口管理，设置中心不再修改这些基础配置。
      </div>

      <div className="remote-im-settings-summary" aria-label="远程 IM 当前配置摘要">
        <div>
          <span>当前状态</span>
          <strong>{config.enabled ? '已开启' : '未开启'}</strong>
        </div>
        <div>
          <span>输出刷新</span>
          <strong>{config.outputFlushIntervalMs} ms</strong>
        </div>
        <div>
          <span>单次回传</span>
          <strong>{config.outputMaxChunkChars} 字符</strong>
        </div>
      </div>
    </section>
  )
}
