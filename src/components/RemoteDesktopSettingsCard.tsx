import type { RemoteDesktopMode } from '../../electron/remote-im/types.js'

export interface RemoteDesktopSettingsCardProps {
  mode: RemoteDesktopMode
  /** 谁能连入就看这一份：IM 好友。没有第二份「允许名单」。 */
  friendUserIds: string[]
  onMode: (mode: RemoteDesktopMode) => void
  /** 是否允许对端操作本机键鼠。与 mode 分开授权。 */
  control: boolean
  onControl: (allow: boolean) => void
}

interface ModeOption {
  value: RemoteDesktopMode
  label: string
  hint: string
}

// 顺序刻意从最收紧排到最放开，默认项（关闭）在最前：让人先看到"不开"是什么样。
const MODE_OPTIONS: ModeOption[] = [
  {
    value: 'disabled',
    label: '关闭',
    hint: '拒绝一切远程查看请求'
  },
  {
    value: 'unattended',
    label: '无人值守',
    hint: '允许列表内的设备可直接查看，不打扰你'
  },
  {
    value: 'attended',
    label: '每次确认',
    hint: '每次请求都要你手动同意（当前版本尚未支持，会直接拒绝）'
  }
]

/**
 * 远程桌面（被控端）设置。
 *
 * 本机只能被查看、不能发起：界面上没有"连接对方"的入口，因为这个能力
 * 在代码里就不存在，不是被藏起来了。
 */
export default function RemoteDesktopSettingsCard(
  props: RemoteDesktopSettingsCardProps
): JSX.Element {
  const allowed = props.friendUserIds.filter((userId) => userId.trim())

  return (
    <section className="ai-settings-card">
      <h3 className="ai-settings-card-title">远程桌面</h3>
      <div className="ai-settings-note">
        允许 MaiChat 查看这台电脑的屏幕。本机只作被控端，不能反过来查看别人。
      </div>

      <div className="remote-desktop-mode-list">
        {MODE_OPTIONS.map((option) => (
          <label key={option.value} className="remote-desktop-mode-option">
            <input
              type="radio"
              name="remote-desktop-mode"
              checked={props.mode === option.value}
              onChange={() => props.onMode(option.value)}
            />
            <span className="remote-desktop-mode-label">{option.label}</span>
            <span className="remote-desktop-mode-hint">{option.hint}</span>
          </label>
        ))}
      </div>

      <div className="ai-settings-note">
        {allowed.length > 0 ? (
          <>允许连入的设备：{allowed.join('、')}（沿用 IM 好友列表）</>
        ) : (
          // 白名单空 = 谁都连不进来。不明说的话，用户会以为开了就能用。
          <>⚠ IM 好友列表为空，当前没有任何设备能连入。</>
        )}
      </div>

      {props.mode !== 'disabled' && (
        <>
          {/* 控制是比查看更重的一档授权，所以单独一个开关、默认关闭：
              开了"看屏幕"不等于把整台电脑交出去。 */}
          <label className="remote-desktop-control-toggle">
            <input
              type="checkbox"
              checked={props.control}
              onChange={(event) => props.onControl(event.target.checked)}
            />
            <span className="remote-desktop-mode-label">允许对方操作我的键盘和鼠标</span>
            <span className="remote-desktop-mode-hint">
              不勾选时对方只能看，不能点击或输入。Win+L 锁屏一律拦截——锁上之后只能你本人到电脑前解锁。
            </span>
          </label>
          <div className="ai-settings-note">
            共享期间界面顶部会常驻红色提示条，无法隐藏——这样你回到电脑前能一眼看出屏幕是否正被查看。
          </div>
        </>
      )}
    </section>
  )
}
