# MaiChat Desktop 等待工具提问时回答按钮内容挤压

- 报告时间与平台：2026-10-17，macOS 26.4、Apple Silicon。用户截图显示创作中心模型调用 `question` 工具后，输入区右下角的圆形按钮同时出现红色停止图标和“回答”文字，文字被裁切。
- 源码基线：`ad37d4463ff954c83aeb37b04f40800c7d8af021`；用户当前运行的安装版是 `/Applications/MaiChat.app` 2.1.2，未在本轮替换。
- 复现：`AgentPanelSessionTest::pendingQuestionIsRestoredOnlyInItsSession` 用假模型触发真实待回答状态。新增按钮无文字的断言在修复前失败：实际 `send->text()` 为“回答”，期望为空。
- 根因：`AgentChatPanel::showQuestion()` 往固定 36×36 的图标按钮写入“回答”文字；按钮仍保留 `setRunning(true)` 的红色停止图标和样式。点击实际会先进入回答分支，因此图标也与动作不符。
- 修复：待回答时按钮保持纯图标，使用蓝色上箭头和“回答问题”悬停/无障碍名称；普通运行状态仍为红色停止图标。会话切走再切回时恢复相同状态，回答后回到运行按钮状态。
- 修复源码提交：`eb038ed5ea7d27468bf37299cbb26aa41003c08b`。
- 验证：同一定向用例修复后通过，回答操作和跨会话恢复仍通过。测试进程的真实 Qt 面板截图已目视检查，回答按钮为完整的圆形箭头，没有文字裁切；未使用真实模型或付费调用。
- 候选 App：`MaiChat/desktop/dist-switch-fix-122a735a/MaiChat-macos-arm64/MaiChat.app`，2.1.2，临时签名校验和启动冒烟通过；可执行文件 SHA-256 `1d8bb21ff3ab4f795f4d25ebf984075adb71173f43b28dc24ea3b985625a8dba`。
- 未验证边界：当前安装版未停止或替换，用户界面尚未更新；Windows 未运行本次修复。
