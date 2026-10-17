# MaiChat Desktop 创作中心切换会话卡顿与根目录工作区

- 报告时间与平台：2026-10-17，macOS 26.4、Apple Silicon。用户在 Desktop 创作中心反复点击左侧会话时明显卡顿；当前会话底部显示只读工作目录 `/`。iOS 同类切换流畅。
- 源码基线：`ad37d4463ff954c83aeb37b04f40800c7d8af021`。现场进程是 `/Applications/MaiChat.app/Contents/MacOS/MaiChat`，版本 2.1.2，可执行文件 SHA-256 为 `fa45284666a182200d77d78bfd0aa35a95627c39089188cc66ef86c9a9e19bcb`；它不是本次源码构建产物。
- 复现步骤：用只读 SQLite 备份取得现场会话快照，在独立测试进程中用真实 `AgentController`、`AgentChatPanel`、`MarkdownView` 依次打开快照中的会话，不连接模型、不发送消息、不修改运行中的 App。含 96 条消息、347 个内容片段的会话，连续两次打开分别耗时 6642 ms、6664 ms；短会话约 13–388 ms。快照中 6 个会话有 2 个保存的工作目录为 `/`。
- 根因证据：`AgentChatPanel::openSession()` 同步执行 `reloadFromStore()`；恢复历史时逐条调用 `MarkdownView::addItem()` 或 `addWidget()`。这些方法每次调用都会执行 `relayoutAll()`，重新排版此前全部内容。会话越长，重复工作按条目数平方增长。工作目录 `/` 来自已持久化的会话字段；本次源码中新会话默认位置原为 Documents，打开旧会话时仍显示其持久化目录。
- 修复：历史恢复期间合并 MarkdownView 更新，全部条目加入后只排版一次；新会话默认创建在 Documents 下的 `MaiChat Workspace` 可写文件夹，Documents 不可用时才退到 App 数据目录，不再使用进程目录或系统根目录。已保存为根目录 `/` 的空闲会话在打开时迁入该文件夹；正在执行任务的会话不在中途改目录，其他自选工作目录不变。
- 修复源码提交：`eb038ed5ea7d27468bf37299cbb26aa41003c08b`。
- 同一快照复测：96 条消息的会话连续两次打开分别耗时 209 ms、186 ms；较短会话约 5–241 ms。此结果证明面板恢复阶段的卡顿显著降低，不代表当前已安装版本已经更新。
- 验证：macOS Release 构建 `maichat`、`agent_controller_test`、`agent_panel_session_test`、`markdown_view_test`、`main_window_layout_test` 通过；相关 CTest 4/4 通过；构建产物 `--smoke` 退出 0。测试覆盖新会话默认目录、只迁移根目录会话、保留自选目录及批量排版与逐条排版的几何一致性。`agent_page_visual_probe` 生成的实际 Qt 窗口截图已检查，消息、工具卡、输入区与列表布局正常。未触发付费模型请求。
- 未验证边界：当前运行中的 `/Applications/MaiChat.app` 未停止或替换；尚未在该安装版内验证修复后的会话切换。Windows 和 iOS 未运行本次变更。
