# MaiChat 跨端对齐增量验收记录

- 时间与基线：2026-10-17 至 18 日；当前 `main` 的 Mac/Desktop 源码提交 `04d023e815eb93099e820218240991ac9dc71451`。iOS、Android、Desktop 的已核对能力和未完成项详见 [`MaiChat/Android与iOS功能对齐.md`](../../MaiChat/Android与iOS功能对齐.md)。
- 用户范围：iOS 当前功能与交互为基准；其它平台全部功能要继续逐项对齐。用户明确无需迁移旧数据。本轮仅为新 Desktop 会话使用账号隔离目录；未删除旧 `agent.db`、旧工作区或用户原媒体文件。
- Mac Desktop：视频 Graphics 黑屏修复、播放器画面上控件、视频分屏、工具调用一行分组、媒体卡片、引用回复、音频卡片、账号隔离工作区与会话库、图片气泡预览过渡、图片/视频草稿和异步账号媒体导入均已改动。Mac 相关 `agent_controller_test`、`agent_panel_session_test`、`main_window_layout_test`、`MaiFfplayVideoDialogTest` 4/4 通过；另有 MaiAgent 模型客户端相关 4/4 CTest 通过。预览过渡、视频分屏与草稿的实际 Qt 画面截图已查看。
- Mac 候选 App：`MaiChat/desktop/dist-parity-04d023e8/MaiChat-macos-arm64/MaiChat.app`，版本 2.1.2，arm64，可执行文件 SHA-256 `111c2e108bd4a04995cc4aecc2c7092ff71b689f7f5d6a61e2e9ee717330a0e1`。封装脚本完成依赖复制及临时签名；`codesign --verify --deep --strict` 和该包的 `--smoke` 通过。
- 当前安装版边界：`/Applications/MaiChat.app` PID 72397 仍运行旧可执行文件 SHA-256 `454989385fd70c2ec1829660e29fbc86b21cd708c897561c22acb7fcb67e0724`。按 `MaiChat/AGENTS.md`，结束并替换正在运行的 App 会中断当前任务，已向用户请求许可；未收到许可前未替换，也不宣称安装版体验已交付。
- Android：在 Android 17 `emulator-5560` 的 Debug APK 上，媒体/聊天/创作中心相关 12 项仪表测试与 Debug 单元测试通过；核对了视频画面、横屏左右栏、视频分屏、实时转写覆盖层、图片放大和连续浏览的截图/录屏。没有连接实体 Android 手机，故没有实机安装结论。
- iOS：Release arm64 模拟器正常 App 启动显示登录页；当前源码构建的 2.2.1 (89) 已覆盖安装到连接的 iPhone 14 Pro Max。设备锁屏拒绝启动验收，详见 [`maichat-ios-2.2.1-install-20261018.md`](maichat-ios-2.2.1-install-20261018.md)。
- Windows 与服务端：本机没有 Windows 构建或运行环境；Windows 安装版未被替换，本轮 Qt 跨端改动未在 Windows 实测。后台服务程序未修改、未重新部署；没有发布新 Release。
