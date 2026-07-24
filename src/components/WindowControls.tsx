// 无边框窗口的页面内自绘窗口按钮（最小化 / 最大化 / 关闭）。
// 与系统 titleBarOverlay 的区别：按钮是 DOM 的一部分、参与正常层级，
// 弹窗可以盖住它，不会出现「系统浮层永远压在界面上」的遮挡问题。
// macOS 用系统红绿灯（titleBarStyle: hidden），此组件不渲染。
export default function WindowControls(): JSX.Element | null {
  if (window.api.platform === 'darwin') return null
  return (
    <div className="window-controls">
      <button
        type="button"
        className="window-controls-btn"
        aria-label="最小化"
        title="最小化"
        onClick={() => window.api.windowControls.minimize()}
      >
        <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
          <line x1="0.5" y1="5" x2="9.5" y2="5" stroke="currentColor" strokeWidth="1.1" />
        </svg>
      </button>
      <button
        type="button"
        className="window-controls-btn"
        aria-label="最大化或还原"
        title="最大化 / 还原"
        onClick={() => window.api.windowControls.toggleMaximize()}
      >
        <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
          <rect x="0.5" y="0.5" width="9" height="9" fill="none" stroke="currentColor" strokeWidth="1.1" />
        </svg>
      </button>
      <button
        type="button"
        className="window-controls-btn window-controls-close"
        aria-label="关闭"
        title="关闭"
        onClick={() => window.api.windowControls.close()}
      >
        <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
          <line x1="0.7" y1="0.7" x2="9.3" y2="9.3" stroke="currentColor" strokeWidth="1.1" />
          <line x1="9.3" y1="0.7" x2="0.7" y2="9.3" stroke="currentColor" strokeWidth="1.1" />
        </svg>
      </button>
    </div>
  )
}
