# MaiAgent Graphics

本目录保留从 OBS Studio 移植的 Graphics、util、Metal 和后端源码。
来源、目录映射与许可见 [`UPSTREAM.md`](UPSTREAM.md)；ARM 使用仓库内的
[`SIMDe`](../third_party/simde/README.md)。对外接口沿用 OBS 的 `gs_*`，
入口头文件为 [`../include/MaiGraphics.h`](../include/MaiGraphics.h)。

启用 `MAIAGENT_BUILD_GRAPHICS=ON` 后：

- macOS Apple Silicon 构建 `MaiAgent::Graphics` 动态库和
  `maiagent_obs_metal` 模块。把模块完整路径交给 `gs_create`。
  `MaiObsMetalRenderTests` 验证了设备创建、离屏清屏、纹理上传和回读。
- iOS 构建静态 `maiagent_graphics` 与 Metal 后端，调用
  `gs_create(..., "builtin:metal", ...)`，避免运行时加载外部模块。
  iOS 模拟器已实测设备创建、离屏清屏与回读；真机 SDK 交叉编译和
  MaiChat iOS Workspace 构建通过。UIKit 交换链呈现尚未实测。
- Android 的 `opengl/` 仍是 OBS 桌面 OpenGL 源码，尚未完成 GLES/EGL
  移植。Windows 的 D3D11 运行时也尚未接通。macOS、iOS、Windows
  均不构建 OpenGL 后端。

`data/` 含 OBS `libobs/data` 的 21 个常用 `.effect` 文件，构建时原样
暂存到 Graphics 产物旁；iOS App 和 Android APK 的资源构建也会打包它们。
macOS Metal 已验证 `default.effect` 和 `solid.effect` 可加载。当前 9 个
矩形纹理及去隔行 effect 在 Metal 上还不能编译，不能把全部资源视为已可用。

FFmpeg 图像解码链接仓库内 9.0.2 源码构建的静态库；macOS 构建启用
VideoToolbox 和 AudioToolbox。独立 Graphics 构建的 OBS 应用层信号目前由
`compat/` 暂时隔离，视频信息固定为 SDR，尚未转发 `video_reset`。
MaiChat 的消息区、AI 助手和播放器尚未切换到此渲染路径；媒体帧导入、
音画同步与窗口生命周期仍需接入。
