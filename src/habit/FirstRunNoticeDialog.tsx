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
          <h3>🎓 Skill 学习已启用</h3>
        </div>
        <div className="habit-firstrun-body">
          <p>
            为了帮你把日常重复操作沉淀成可复用的 prompt 模板，本版本新增了「Skill 学习」智能体。
            它会在你使用本软件期间静默采集少量行为信号，并定期建议候选 skill。
          </p>

          <div className="habit-firstrun-section">
            <strong>会采集</strong>
            <ul>
              <li>主会话发送给 AI 的 prompt 文本</li>
              <li>仓库查看里发送的 prompt 与代码标注</li>
              <li>Diff 审查里的批注</li>
              <li>模板调用与方案导入事件</li>
            </ul>
          </div>

          <div className="habit-firstrun-section">
            <strong>不会采集</strong>
            <ul>
              <li>本软件之外的系统活动（键盘、鼠标、其他进程）</li>
              <li>AI 返回的回复、终端输出、文件内容</li>
              <li>方案文件正文与代码本身</li>
            </ul>
          </div>

          <div className="habit-firstrun-section">
            <strong>数据去向</strong>
            <ul>
              <li>原始事件只存本机 SQLite，默认保留 90 天</li>
              <li>仅在生成候选 skill 时把最小聚合摘要发给你已配置的主会话 AI CLI</li>
              <li>可随时在「🎓 Skill 学习 → 采集」里关闭采集、清空数据</li>
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
