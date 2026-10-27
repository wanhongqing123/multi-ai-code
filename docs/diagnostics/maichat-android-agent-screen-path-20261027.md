# Android Agent 截屏输出路径与宿主范围不一致

## 现场

- 2026-10-27，Android 17 / API 37 隔离模拟器；源码基线 `e8fa53ea6`。
- Android `AIAssistantController` 把 `appRoot` 设为 App 私有数据目录，例如
  `/data/user/0/com.kongshang.maichat.isolated`。C++ 的 `mobile_capture_screen`
  原先请求写入 `appRoot/screenshots/*.png`。
- Java `MobileScreenCaptureTools` 只允许 `getNoBackupFilesDir()` 下的 PNG，
  即 `appRoot/no_backup/...`。两处路径范围不相交，故 Agent Tool 必然收到
  「截图目标必须位于 App 缓存目录」；之前的测试只直接调用采集服务，未覆盖工具入口。

## 修正与验证

- 修复提交 `c5aee885fe57d45c1ed9f5da160099f3fdde97fa`。
- Android 专用的 C++ 工具输出目录改为 `appRoot/no_backup/screenshots`，复用
  Java 宿主已有的私有路径校验；不放宽宿主允许的写入范围。
- `MobileScreenCaptureInstrumentedTest` 从系统整屏选择器采集其他 App 后，
  经 `MaiChatHostTools.execute("mobile_capture_screen", ...)` 验证旧路径被拒绝、
  新路径成功写入 PNG，且图像内容仍是目标设置页。
- 隔离 Debug App 与 AndroidTest APK 重链成功；定向测试 1/1 PASS。

未在实体 Android 设备及真实付费 Agent 轮次上验证。测试使用隔离包名，
没有写入用户的正式 MaiChat App 数据。
