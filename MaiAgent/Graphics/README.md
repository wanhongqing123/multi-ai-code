# MaiAgent Graphics

这里移植了 OBS 的 Graphics、util、Metal 和 OpenGL 源码。版本、目录映射、许可
与平台适配见 [`UPSTREAM.md`](UPSTREAM.md)。对外接口是 OBS 原有的 `gs_*`，
入口在 [`../include/MaiGraphics.h`](../include/MaiGraphics.h)。图片加载的共用入口
是 [`../include/MaiGraphicsImageRenderer.h`](../include/MaiGraphicsImageRenderer.h)：
FFmpeg 解码并缩放，Graphics 上传到纹理、复制到渲染目标并回读 RGBA8。
调用同步且应放在后台线程；图形设备按物理线程复用。

启用 `MAIAGENT_BUILD_GRAPHICS=ON` 后：

- macOS Apple Silicon 构建 `MaiAgent::Graphics` 动态库及 Metal 模块。
  把模块完整路径传给 `gs_create`。离屏清屏、纹理上传、回读、FFmpeg 图片
  和常用 effect 的测试已通过；MaiChat.app 会打包两项库及 effect 资源。
- iOS 静态链接 Metal，使用 `gs_create(..., "builtin:metal", ...)`。
  模拟器实测离屏渲染和图片加载；真机 SDK 交叉编译与 MaiChat Workspace
  构建通过。
- Android 构建 Graphics 和 GLES 两个 `.so`。arm64 模拟器实测 GLES 3.1
  设备创建、离屏渲染、FFmpeg 图片和回读；Debug APK 含两种 ABI 的库及
  effect 资源。Android 消息图片加载器的设备测试通过。
- Windows 应使用 D3D11；运行时尚未接通。macOS、iOS 和 Windows 均不使用
  OpenGL。Linux 的 X11/Wayland OpenGL 实现源码保留，但当前未进入 MaiAgent
  构建。桌面 Qt 图片加载逻辑共用，后端由平台选择。

OBS `libobs/data` 的 21 个 `.effect` 文件位于 `data/` 并进入三端资源包。
macOS Metal 已验证 `default.effect` 和 `solid.effect`；目前有 9 个矩形纹理
或去隔行 effect 在 Metal 上不能编译，不能视为全部可用。

Mac 构建的 FFmpeg 9.0.2 启用 VideoToolbox 和 AudioToolbox。独立 Graphics
构建仍用 `compat/` 隔离 OBS 应用层信号，视频信息固定为 SDR，尚未转发
`video_reset`。macOS、iOS、Android 的聊天图片加载入口已优先调用共用图片
渲染函数，失败时回退原生解码。Android GLES 的 shader/effect 与窗口交换链、
iOS UIKit 交换链都未做真实界面呈现验收；视频播放器的媒体帧导入、
音画同步和生命周期也尚未切换到 Graphics。
