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
            这一版仅会按周期保存后台屏幕截图样本，用于后续回看和分析工作上下文。
            你可以随时在设置里暂停、关闭或清空采集数据。
          </p>

          <div className="habit-firstrun-section">
            <strong>会采集</strong>
            <ul>
              <li>按周期保存的屏幕截图样本</li>
            </ul>
          </div>

          <div className="habit-firstrun-section">
            <strong>不会采集</strong>
            <ul>
              <li>系统级键鼠原始事件</li>
              <li>前台窗口标题、进程名和应用内 prompt / 命令 / 批注</li>
              <li>密码、令牌、Cookie、支付信息和敏感输入原文</li>
              <li>AI 回复、终端输出和代码正文的全文镜像</li>
            </ul>
          </div>

          <div className="habit-firstrun-section">
            <strong>数据去向</strong>
            <ul>
              <li>截图样本默认只保存在本机</li>
              <li>可随时在“🧠 习惯监控 → 原始采集”里关闭采集、清空数据</li>
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
