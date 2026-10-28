# Agent「完全访问」仍要求一般操作审批

- 时间：2026-10-28，macOS 26.4；关联源码：`6f3d0222a`，修复：`f707ba43`。
- 复现：以 `MaiApprovalPolicy::Never` 运行一个 `requiresPerCallApproval()` 为 true 的非付费工具（测试中为 `external_send`）。调用前仍产生 `PermissionAsked`，与“完全访问”预期不符。
- 根因：`MaiTurnRunner::toolNeedsApproval()` 先检查逐次审批标记，再检查 `Never`。该顺序使工具的逐次标记覆盖了用户选定的完全访问模式。
- 修复：付费提交单独使用 `requiresPaidApproval()`；该标记在任何模式下逐次确认，也不接受“本会话允许”。完全访问对其他工具跳过 Agent 审批；请求批准与帮我批准保持原有逐次审批行为。明确标记的付费动作包括万相、可灵、Seedance、Seedream、GLM、MiniMax 的生成提交，以及方舟真人认证会话启动。后者[官方文档](https://docs.volcengine.com/docs/ark/h5-create-visual-validate-session-api?lang=zh)称当前限时免费，接口会返回计费状态，因此保留确认。
- 测试：`MaiPermissionTests` 验证完全访问下非付费工具连调两次无弹窗，付费工具连调两次仍分别弹窗；MaiAgent 定向 8/8、Mac Desktop 定向 9/9 通过。Mac 构建目录的 `MaiChat.app --smoke` 退出 0，版本 2.2.30，Qt 5.15.18。iOS 隔离模拟器权限菜单 UI 测试通过并检查截图；Android 模拟器 Debug App 显示三档选项和说明，切换到完全访问后菜单显示新状态。Android 首版把说明作为 `AlertDialog` message 导致选项被隐藏；改为 custom view 后同一界面可见。iOS 测试专用入口原先缺少 `RemoteIMAppState` 而启动崩溃，修复入口后测试通过。
- 未验证：iPhone 真机当前不可用；Windows 本轮未编译或运行；Mac 电脑操作工具缺少系统录屏与辅助功能授权，真实点击/输入未验。没有执行任何真实付费提交，也没有替换用户当前运行的 App。

边界：此规则只识别内置工具明确声明的付费提交。普通聊天模型消耗、任意 shell/HTTP 调用可能产生的第三方费用，无法从通用工具参数可靠判定。
