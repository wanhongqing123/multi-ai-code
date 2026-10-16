# Android 创作中心长消息定位测试误报（2026-10-16）

- 平台：Mac 上的 Android ARM64 模拟器，已安装 `a5123a7a` 的 Debug App 和 androidTest APK。
- 复现：运行 `AIAssistantInstrumentedTest.streamsUnicodeStopsAndKeepsNativeComposer`，
  在键盘打开后发送 `long`，等待 45 段长回复；原测试在第 400 行等待末尾位置时超时。
- 现场：12 秒和 18 秒截图均显示 `Tail marker`、消息元信息及键盘，没有消息区底部空白。
  测试诊断提交 `632b8e75` 记录：`count=9`、`firstVisible=8`、`lastVisible=8`、
  `listHeight=1156`、`paddingBottom=63`、`lastTop=-4863`、`lastBottom=1140`、
  `lastHeight=6003`、`imeVisible=true`。
- 根因：列表设置了 `clipToPadding(false)`，最后一行允许显示在底部 padding 中；
  原断言却要求 `lastBottom <= listHeight - paddingBottom + 2`，即 `<=1095`。
  实测 `lastBottom=1140` 仍在列表实际边界 `1156` 内，故这是测试误报。
- 修复：`c0b6eb95` 只在 androidTest 中依据 `getClipToPadding()` 计算可见边界，
  保留超时时的几何诊断；产品 `ListView` 行为未修改。
- 验证：Mac 端在同一 ARM64 模拟器重建并安装 androidTest APK 后，原失败用例
  `OK (1 test)`，耗时 7.763 秒；`composerMatchesIosInteractionStructure` 与
  `mediaDraftWaitsForSendAndStaysInItsSession` 也分别通过。Windows 本机没有 ARM Android
  设备，未在 Windows 自行运行 APK；Android 物理真机和横屏体验需另行确认。
