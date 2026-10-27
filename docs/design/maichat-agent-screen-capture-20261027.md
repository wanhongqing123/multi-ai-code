# MaiChat Agent 截屏工具

## 用户确认的范围

2026-10-27 用户明确选择「整台设备当前屏幕」，包括其他 App。截屏结果应作为
图片交给 Agent 观察；不能把仅有 PNG 文件、全白画面或 MaiChat 自身画面误报为
已截取目标 App。用户决定先完成 Android、Mac、Windows；iOS 27 等开发机升级。
当前产品变更提交为 `c5aee885fe57d45c1ed9f5da160099f3fdde97fa`。

## 平台路径

| 平台 | 入口与系统接口 | 当前边界 |
|---|---|---|
| Windows | 现有 `screenshot` 普通 API／Agent Tool，使用 WGC 获取显示器或可见窗口单帧 | 需交互桌面；锁屏、安全桌面、最小化窗口不承诺 |
| macOS | 同一 `screenshot` 工具，ScreenCaptureKit `SCShareableContent` 与 `SCScreenshotManager` | macOS 14+，需系统屏幕录制权限，授权后可能需重启 App |
| Android | `mobile_start_screen_share` 调系统整屏选择器，`mobile_capture_screen` 读取 MediaProjection 最近一帧，`mobile_stop_screen_share` 结束 | 由用户选择「Share entire screen」；返回 MaiChat 后读取目标 App 的缓存帧 |
| iOS 27 | 待接 `SCContentSharingPicker` 与 `SCStream`，由用户选整个显示器 | 当前 Mac 只有 iOS 26.5 SDK；尚未实现、编译或真机验收 |

Windows 选择 WGC，因为同一 API 可捕获[显示器和应用窗口的单帧](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)。
DXGI Desktop Duplication 面向[连续桌面帧](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)，
`PrintWindow` 要求目标窗口响应同步绘制且[调用可能阻塞](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-printwindow)。
本功能不在 WGC 失败时暗中切换到 GDI、DXGI 或 PrintWindow。

Apple 的 [iOS 27 ScreenCaptureKit 示例](https://developer.apple.com/documentation/screencapturekit/capturing-screen-content-on-ios)
提供系统整屏选择器和后台 `screen-capture` 模式。本机 Xcode 26.6 只含 iOS
26.5 SDK；正式 Xcode 27 [要求 macOS 26.6 起](https://developer.apple.com/xcode/system-requirements)。
用户会自行升级开发机，因此 iOS 整屏能力不以 App 内截图冒充。iOS Agent 在
宿主真正接通前不注册这组移动截屏工具。

## 授权与交付

- Windows／Mac `screenshot` 每次调用均走 Agent 审批；Mac 首次还需系统屏幕
  录制授权。Android 每次开始共享时由系统选择器征得用户同意，共享服务持续
  显示前台通知，用户可停止。
- Agent 截图写入本机工作区，返回可供模型读取的 PNG 图片；不自动发给 IM 联系人。
- Android 真实画面回归见
  [排障记录](../diagnostics/maichat-android-screen-capture-return-blank-20261027.md)。
- 当前 Mac 测试进程没有屏幕录制权限，已验证工具注册／权限拒绝和桌面 App
  链接，实际像素内容待用户授权后复验。iOS 27 待 SDK 与可用真机。
