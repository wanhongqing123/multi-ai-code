# Desktop 创作中心删除会话确认框回归（2026-10-23）

- **现场**：用户在 Mac Desktop 2.2.14 的创作中心，点本机会话右侧 `…` → 删除对话。确认框出现系统问号图标、英文 `No/Yes`，未命名会话显示核心占位标题 `New session`。用户截图见本次会话 2026-10-23 21:07 附图。
- **复现基线**：源码 HEAD `8ac971d3104ae447f965e8fb6ea0c0d479e4f248` 加当时工作树改动；测试平台 macOS 26.4，Qt 5.15.18 arm64。运行中的用户 App 未替换。
- **根因证据**：`MainWindow::rebuildAgentPage` 中 `AgentSessionList::deleteSessionRequested` 回调直接调用 `QMessageBox::question`，绕过已有的 `AppMessageDialog`；正文直接读取 `session.title`，而未命名会话的核心占位标题是 `New session`。复用组件首次真实截图还发现 Mac 的取消/删除按钮几何相互重叠 2 个逻辑像素。
- **修复**：删除确认改用 `AppMessageDialog::confirm` 的危险操作模式，标题映射使用 `session.isUntitled()`，按钮为中文 `取消`/`删除`，取消是默认焦点。共用组件为两个确认按钮加入显式间隔。删除提交、Busy 报错及数据流保持原有路径。
- **验证**：`main_window_layout_test agentSessionDeletionUsesChineseProductDialog` 实际构造主窗口和未命名会话，触发删除信号，检查弹窗文案、按钮间距、取消后会话存在、确认后会话消失；这一个业务用例通过。`app_message_dialog_test` 的五个组件用例通过；`git diff --check` 通过。截图 `/tmp/maichat-delete-confirmation-final.png` 已人工查看，确认没有问号图标、英文按钮或按钮贴边。
- **最终候选**：`MaiChat/desktop/dist-session-room-20261023-v2215/MaiChat-macos-arm64/MaiChat.app`，版本 2.2.15，主可执行文件 SHA-256 `ebc4c116377261405ebbf1e597ceb5d6588863aa22cc2b7e7550c499869169b0`。独立签名、启动检查和 Desktop 三组 CTest 3/3 通过。
- **边界**：验证的是本机 Mac 的新构建测试目标；用户正在运行的 App 尚未替换。没有改变远端会话删除、iOS 或服务端。
