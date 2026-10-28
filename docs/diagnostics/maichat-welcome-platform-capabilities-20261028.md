# 创作中心欢迎页遗漏各端已注册能力

- 现场：2026-10-28，用户发来 iOS 27.2 创作中心新对话截图，欢迎页仍只有旧的 12 张卡片，没有近期加入的截屏、浏览器、定时任务、日历和二维码等入口。修复前基线为 `3446aefaa14315007b206f6f71030b0d732bfd74`。
- 根因证据：iOS `AIAssistantView.welcome`、Desktop `AgentChatPanel` 和 Android `AIAssistantPanel.MessageRow` 各自维护静态展示文案；近期工具已在各端 Agent 注册，欢迎页没有同步更新。Desktop 还没有突出终端与电脑操作；iOS 原“电话与短信”未区分不能读取系统短信的边界。
- 修复：按平台实际注册能力分别更新三端欢迎页。iOS 仅在真机 iOS 27+ 展示整屏截屏，并分别写明系统日历、位置、手机通讯录与拨号发短信；Desktop 展示终端与进程、电脑操作、日历导入等桌面能力；Android 展示其整屏截屏及系统权限相关能力。iOS 长列表初始锚点改为顶部，消息历史仍从底部定位。修复模拟器欢迎页测试入口缺少 `RemoteIMAppState` 的注入。
- 验证：macOS Desktop `agent_panel_session_test` 1/1 通过，亲看 1100px 和 700px 截图，卡片标题无裁切且窄宽度可滚动；Android Debug APK 构建成功、正常 App 启动到登录页、`composerMatchesIosInteractionStructure` 真模拟器测试 1/1 通过并亲看欢迎页截图；iOS 26.5 模拟器欢迎页 UI 测试 1/1 通过，亲看首屏及滚动末尾截图；iOS 真机目标 Debug 签名构建与 UI 测试目标编译通过。
- 边界：iOS 27.2 真机在本轮欢迎页最终实机截图前暂时断开，真机最终布局仍待重连回看；Windows 端未在本机执行。截图工具的 iOS 27.2 真机跨 App 取帧、附件预览和停止共享已在前一轮独立验收通过。修复提交见本文件的 Git 历史。
