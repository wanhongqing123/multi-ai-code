# Desktop 新对话缺少创作中心介绍

- 时间与平台：2026-10-28，macOS 26.4、Qt 5.15.18；用户提供的 Desktop 与 iOS 同一新对话截图作对照。
- 基线源码：`2dd2021d484bce34f34db983f3a1e24ca49c14ef`，Desktop 2.2.31。用户截图中 Desktop 主区域为空白，iOS 显示「从这里开始创作」和两列功能介绍。
- 根因：`AgentChatPanel` 的对话区只装入 `MarkdownView`；`reloadFromStore()` 清空新会话的零条消息后，没有空态视图。iOS `AIAssistantView.welcome` 则单独渲染该介绍。
- 修复：在 Desktop 对话区叠加静态欢迎视图，按可用宽度把 12 项当前 Desktop 能力排成四、三或两列；第一条消息或运行中状态出现时显示原 `MarkdownView`，清空或切回另一空会话时恢复欢迎视图。小窗口可滚动查看，输入区和原消息视图不重建。
- 验证：Mac 上新用例检查 12 张介绍卡、宽 1100 时四列、宽 700 时三列、窄窗两列、空会话显示、首条消息后隐藏及切换回空会话恢复；同用例 3/3 QtTest 检查通过。`agent_panel_session_test` 与 `main_window_layout_test` 完整 CTest 2/2 通过。已亲看宽 1100 和紧凑 700 的面板截图；正式全功能 Release 与 Windows 打包仍需以本修复提交重新验收。

边界：介绍卡与 iOS 一样用于说明能力，不是立即执行工具的按钮；Desktop 未列出仅手机可直接使用的系统相册与电话短信入口。
