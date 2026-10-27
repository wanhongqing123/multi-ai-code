# Android 整屏共享返回 MaiChat 后偶发空白帧

## 现场与复现

- 2026-10-27，Android 17 / API 37，`sdk_gphone16k_arm64` 模拟器；隔离 Debug App
  `com.kongshang.maichat.isolated`，源码基线 `e8fa53ea6`。
- 在 `MobileScreenCaptureInstrumentedTest` 中同意系统的「Share entire screen」，
  打开 Android 设置约 3 秒，再返回 MaiChat，读取服务缓存的最新 PNG。
- 原测试只验证 PNG 存在及尺寸，报告 PASS；人工查看首次导出的
  `/private/tmp/maichat-android-screen-share-20261027.png`，内容几乎空白，
  只剩状态栏（SHA-256 `cb6c166be76d102fe98f21f60df9c8d2e1a06d056d3ba503b96800ad23f263df`）。
- 增加返回前的取帧诊断后，设置页本身可完整采集，PNG SHA-256 为
  `ea8840e4734efe1a6ef8d31495dc71b41296258c845abf752022b65b0b2c34a4`。
  因此系统整屏授权和采集工作正常，问题集中在返回 MaiChat 时缓存最后一帧。

## 原因与修正

`MobileScreenCaptureService.writeFrame` 原先只在 PNG 压缩前检查
`appForeground`。前后台切换可能发生在压缩期间；即使 `onResume` 已标记
MaiChat 回到前台，该回调仍会把返回动画的空白帧写入 `latestFrame`。
这是由代码时序和前后两张实际 PNG 支持的竞态解释，未取得逐帧系统时间戳。

修复提交 `c5aee885fe57d45c1ed9f5da160099f3fdde97fa`。修正是在替换 `latestFrame`
的同一锁内再次检查 `appForeground`；已回到
MaiChat 时丢弃在途帧。测试同时保留返回前设置页 PNG，并抽样比较返回后
工具读到的画面，避免仅凭“PNG 非空”误判。

## 验证与边界

- 隔离模拟器上修正后该用例连续 8 次 PASS；返回后的设置页 PNG 已人工查看，
  SHA-256 `94db7d049e8e38cba51473e07457381d3e372a7348322e59ff76edfa0bf2766a`。
- 系统共享结束后，测试确认服务停止；`onDestroy` 负责删除缓存文件，
  此项未单独断言。未在实体 Android 手机上验证；
  受系统保护的画面仍可能按平台规则显示空白。
- 此项只修正返回时覆盖缓存的竞态，不改变系统授权流程和采集范围。
