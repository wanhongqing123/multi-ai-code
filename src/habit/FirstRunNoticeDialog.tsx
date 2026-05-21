interface Props {
  onAcknowledge: () => void
  onDisableCollection: () => void
}

export default function FirstRunNoticeDialog(props: Props): JSX.Element {
  const { onAcknowledge, onDisableCollection } = props
  return (
    <div className="modal-backdrop">
      <div className="modal habit-firstrun-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>🧠 习惯监控已启用</h3>
        </div>
        <div className="habit-firstrun-body">
          <p>
            这一版会根据你的应用内操作和托管 Chrome 行为，持续整理低风险习惯流程。
            默认会自动启用低风险流程和轻量界面个性化，你也可以随时在设置里关闭采集或自动行为。
          </p>

          <div className="habit-firstrun-section">
            <strong>会采集</strong>
            <ul>
              <li>应用内的主会话 prompt、面板打开、动作触发和 Diff 批注</li>
              <li>仅由本应用拉起的托管 Chrome 访问、点击和输入提示</li>
              <li>低风险站点流程、应用流程和界面调整的候选信号</li>
            </ul>
          </div>

          <div className="habit-firstrun-section">
            <strong>不会采集</strong>
            <ul>
              <li>托管 Chrome 之外的浏览器行为或系统级键鼠活动</li>
              <li>密码、令牌、Cookie、支付信息和敏感输入原文</li>
              <li>AI 回复、终端输出、代码正文和默认浏览器资料</li>
            </ul>
          </div>

          <div className="habit-firstrun-section">
            <strong>数据去向</strong>
            <ul>
              <li>原始事件只存本机 SQLite，默认保留 90 天</li>
              <li>低风险流程默认自动启用，高风险候选仍需要人工确认</li>
              <li>可随时在「🧠 习惯监控 → 原始采集」里关闭采集、清空数据</li>
            </ul>
          </div>
        </div>
        <div className="drawer-actions habit-firstrun-actions">
          <button className="drawer-btn" onClick={onDisableCollection}>
            先关闭采集
          </button>
          <button className="drawer-btn primary" onClick={onAcknowledge}>
            我知道了
          </button>
        </div>
      </div>
    </div>
  )
}
