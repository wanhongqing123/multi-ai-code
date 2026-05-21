import type { ManagedChromeState } from '../../electron/preload'

interface Props {
  state: ManagedChromeState
  busy?: boolean
  onStart: () => void | Promise<void>
  onFocus: () => void | Promise<void>
  onStop: () => void | Promise<void>
}

export function getManagedChromeStatusLabel(state: ManagedChromeState): string {
  if (!state.running) return '未启动'
  if (state.port) return `运行中 · 端口 ${state.port}`
  return '运行中'
}

export default function ManagedChromePanel(props: Props): JSX.Element {
  const { state, busy = false, onStart, onFocus, onStop } = props

  return (
    <section className="habit-settings-section habit-managed-chrome-card">
      <header className="habit-settings-section-head">
        <div>
          <strong>托管 Chrome</strong>
          <div className="habit-settings-hint">{getManagedChromeStatusLabel(state)}</div>
        </div>
        <span
          className={`habit-flow-badge ${state.running ? 'habit-flow-badge-active' : 'habit-flow-badge-idle'}`}
        >
          {state.running ? '监控中' : '待启动'}
        </span>
      </header>

      <div className="habit-managed-chrome-meta">
        <div>
          <span className="habit-managed-chrome-label">独立资料目录</span>
          <code>{state.profileDir ?? '将由应用自动创建'}</code>
        </div>
        {state.lastActiveUrl && (
          <div>
            <span className="habit-managed-chrome-label">最近活跃网址</span>
            <code>{state.lastActiveUrl}</code>
          </div>
        )}
      </div>

      <div className="drawer-actions">
        <button
          type="button"
          className="drawer-btn drawer-btn-primary"
          disabled={busy || state.running}
          onClick={() => void onStart()}
        >
          启动
        </button>
        <button
          type="button"
          className="drawer-btn"
          disabled={busy || !state.running}
          onClick={() => void onFocus()}
        >
          聚焦
        </button>
        <button
          type="button"
          className="drawer-btn warn"
          disabled={busy || !state.running}
          onClick={() => void onStop()}
        >
          停止
        </button>
      </div>
    </section>
  )
}
