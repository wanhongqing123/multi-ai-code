import type { RemoteImConfig } from '../../electron/preload.js'

export interface RemoteImSettingsSectionProps {
  config: RemoteImConfig
  disabled: boolean
  onChange: (next: RemoteImConfig) => void
}

function parseAllowedUsers(value: string): string[] {
  return value
    .split(/[,\n]/)
    .map((item) => item.trim())
    .filter(Boolean)
}

export default function RemoteImSettingsSection(props: RemoteImSettingsSectionProps): JSX.Element {
  const { config, disabled, onChange } = props
  return (
    <section className="ai-settings-card remote-im-settings-section">
      <div className="ai-settings-title">远程 IM</div>
      <div className="ai-settings-note">
        开启后，白名单手机消息会自动发送到当前 AICLI。UserSig 由服务端签发。
      </div>

      <label className="ai-settings-checkbox">
        <input
          type="checkbox"
          disabled={disabled}
          checked={config.enabled}
          onChange={(event) => onChange({ ...config, enabled: event.currentTarget.checked })}
        />
        启用腾讯 IM
      </label>

      <div className="remote-im-settings-grid">
        <label>
          SDKAppID
          <input
            disabled={disabled}
            value={config.sdkAppId ?? ''}
            onChange={(event) =>
              onChange({
                ...config,
                sdkAppId: event.currentTarget.value ? Number(event.currentTarget.value) : null
              })
            }
            placeholder="1400000000"
          />
        </label>
        <label>
          桌面端 UserID
          <input
            disabled={disabled}
            value={config.desktopUserId}
            onChange={(event) =>
              onChange({ ...config, desktopUserId: event.currentTarget.value })
            }
            placeholder="desktop_bot"
          />
        </label>
        <label className="remote-im-settings-grid-full">
          UserSig 服务地址
          <input
            disabled={disabled}
            value={config.userSigEndpoint}
            onChange={(event) =>
              onChange({ ...config, userSigEndpoint: event.currentTarget.value })
            }
            placeholder="https://example.com/tencent-im/usersig"
          />
        </label>
        <label className="remote-im-settings-grid-full">
          允许控制的 UserID
          <textarea
            disabled={disabled}
            value={config.allowedUserIds.join('\n')}
            onChange={(event) =>
              onChange({ ...config, allowedUserIds: parseAllowedUsers(event.currentTarget.value) })
            }
            placeholder="phone_admin"
            rows={3}
          />
        </label>
      </div>
    </section>
  )
}
