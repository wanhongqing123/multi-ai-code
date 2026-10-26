# Android 16 KB 页面兼容：CameraX 原生库

- 时间：2026-10-26，macOS 构建机，Android 17 / API 37 `sdk_gphone16k_arm64` 模拟器。
- 源码基线：`8ac971d3104ae447f965e8fb6ea0c0d479e4f248`，工作区另有未提交的 MaiChat 改动。本记录只归因于下面列出的 CameraX 和构建工具改动。
- 复现：模拟器 `adb shell getconf PAGE_SIZE` 返回 `16384`。此前隔离 APK `MaiChat/android/MaiChat/dist-cloud-agent-v232/MaiChat-android-isolated-production-canary-2.2.3-232.apk` 内 `lib/arm64-v8a/libimage_processing_util_jni.so` 的四个 `PT_LOAD` 段 `p_align` 均为 `4096`，达不到 Android 的 16 KB 要求；此前审计在首次启动时观察到兼容模式提示。

## 原因与修复

`MaiChat/android/MaiChat/app/build.gradle` 的 `camera-core`、`camera-camera2`、`camera-lifecycle`、`camera-view` 固定在 CameraX `1.3.4`。统一改为 `1.4.2` 后，CameraX 的 `libimage_processing_util_jni.so` 和新增的 `libsurface_util_jni.so` 均具有 `16384` 的 `PT_LOAD p_align`。

首次构建在 AGP `7.4.2` 自带的 D8 `4.0.52` 中对 `camera-view-1.4.2` 的 `CameraController$2.class` 报 `NullPointerException`。在 `MaiChat/android/MaiChat/settings.gradle` 按 [R8 官方说明](https://r8.googlesource.com/r8/+/refs/heads/main/README.md#replacing-r8-in-agp) 覆盖为 `com.android.tools:r8:8.6.17` 后，未升级整个 AGP/Gradle 构建链，JDK 11 与 JDK 17 均构建成功。

## 验证

- `bash gradlew :app:assembleDebug :app:assembleDebugAndroidTest -PmaichatIsolatedDebug=true -PmaichatLocalUiAuth=true`：成功。JDK 11 再构建也成功。
- 最终隔离 APK 中 15 个 arm64 `.so` 的所有 `PT_LOAD p_align >= 16384`；`zipalign -c -P 16 -v 4` 返回 `Verification successful`。APK 将 `.so` 压缩打包。
- 最终 APK 安装到 `emulator-5554` 成功；`FriendQrInstrumentedTest` 2/2 PASS。实看扫码页截图 `/tmp/maichat-camerax-16k-scanner-late.png`，可见相机实时预览、扫描框和提示；实看主 App 登录页，无兼容模式弹窗。
- 按 [Android 官方 16 KB 文档](https://developer.android.com/guide/practices/page-sizes) 临时设定 `bionic.linker.16kb.app_compat.enabled=fatal`、`pm.16kb.app_compat.disabled=true`，重新安装 APK 后，扫码测试仍 2/2 PASS，主 App 可启动并保持进程。两项模拟器属性随后恢复为原来的空值。
- 最终隔离 APK：`MaiChat/android/MaiChat/dist-cloud-agent-v232-camerax16k/MaiChat-android-isolated-camerax16k-2.2.3-232.apk`，174460975 B，SHA-256 `e15f9fdcb1a35a456dde803e694183d2f7bde16e8bda32c90fcfd15a731c9770`。

## 边界

本轮仅在 Android 17 的 16 KB 模拟器完成运行验证，未在真实 16 KB 设备、其他系统版本或签名发布包复测。ELF 中 `GNU_RELRO` 的 `p_align=1` 不能单独当作不兼容的证据；Android 文档还要求检查 RELRO 与包含它的 `LOAD` 段及后续可写段的关系。严格兼容模式下的实际启动和相机运行测试已经覆盖当前模拟器，但不替代其他设备的完整验收。
