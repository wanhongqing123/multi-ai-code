# 跨设备命令拒绝后草稿丢失（2026-10-20）

## 复现与证据

- 平台：iPhone 17 Pro / iOS 26.4 模拟器，正常 MaiChat Debug App，
  本机 `services/auth/session-sync-ui-fixture.js` 提供同账号假 Mac Session。
- 在创作中心打开假 Mac 会话，发送 `请拒绝此操作`；假 Mac 返回
  `command_result.ok=false`。页面显示“所属设备拒绝了操作”，输入框却为空。
- 首次失败见 `/tmp/maichat-session-sync-retry-ui.xcresult`。第二次修正后同一
  操作仍失败，见 `/tmp/maichat-session-sync-retry-ui-2.xcresult`。
- 定点模拟器日志显示，发送回调清空输入后恢复代次确已变化，而 SwiftUI
  `onChange` 回调读到旧快照中的空 `remoteRestoreText`。这不是假 Mac 未回执；
  界面已显示其拒绝结果。

## 修正与验证

- 命令在发出前登记操作 ID。网络结果未知时重试同一操作复用 ID，防止重复提交；
  所属设备确认后移除 ID。收到拒绝或等待超时则保留/恢复输入草稿。
- iOS 的 `onChange` 从当前模型读取恢复文本，并处理回执与发送回调交错的顺序。
  Android 同样为回执与发送回调保存恢复代次；Mac/Windows 面板收到拒绝或
  等待超时时恢复原文本。
- 同一正常 iOS App 用例红转绿：
  `MaiChatUITests/AIAssistantUITests/testNormalAppReadsAndControlsLocalFakeMacSession`
  读取假 Mac 历史、正常发送消息、再触发拒绝并检查原文仍在输入框。
  通过结果为 `/tmp/maichat-session-sync-retry-ui-4.xcresult`；已人工查看拒绝态截图。
- Android Debug/Release/AndroidTest Java 编译和 146 项单测通过；Mac 桌面
  `desktop_session_sync_host_test` 与 `main_window_layout_test` 通过。

## 边界

本机假 Mac 没有模型、工具或真实账号数据。Android 真机、iOS 真机与真实
Mac/Windows 跨网络互控、阿里云服务部署仍需分别验收。`send` 入队成功只表示
投递成功；未知结果不得当作执行成功或自动重新创建付费任务。
