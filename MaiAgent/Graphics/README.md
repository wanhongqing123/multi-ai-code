# MaiAgent Graphics

本目录保留从 OBS Studio 移植的 Graphics、util 和各后端源码。来源、目录映射与
许可见 [`UPSTREAM.md`](UPSTREAM.md)；ARM 上使用仓库内的
[`SIMDe`](../third_party/simde/README.md)。对外暴露 OBS 原有的 `gs_*` 接口，
入口头文件是 [`../include/MaiGraphics.h`](../include/MaiGraphics.h)。

启用 `MAIAGENT_BUILD_GRAPHICS=ON` 后，macOS Apple Silicon 构建
`MaiAgent::Graphics`（实际目标 `maiagent_graphics`）动态库和
`maiagent_obs_metal` 模块。调用方链接动态库，把 Metal 模块的完整路径传给
`gs_create`；资源调用在 `gs_enter_context` / `gs_leave_context` 之间执行。
`MaiObsMetalRenderTests` 已验证从创建 Metal 设备，到离屏纹理清屏、暂存与
RGBA 像素回读的完整路径。FFmpeg 图像解码代码链接仓库内 9.0.2 静态库。

当前平台方向是：macOS 和 iOS 使用 Metal，Windows 使用 D3D11，Android
使用 OpenGL ES。复制的桌面 `opengl/` 源码尚未接入 Android；macOS 和
Windows 不构建 OpenGL 后端。iOS、Android 和 Windows 的 Graphics 运行时
仍未接通，当前只有源码编译检查或后端源码快照。

macOS 的独立 Graphics 构建尚未接入 OBS 应用层。`compat/obs.h` 和
`compat/MaiObsStandalone.c` 暂时让 Metal 后端拿到默认 SDR 视频信息，
并且不转发 OBS 的 `video_reset` 信号；因此 HDR/设备重置仍需宿主桥接。
当前也未将 MaiChat 消息区、AI 助手或播放器的现有渲染切换到此库，
媒体解码帧导入、音画同步、窗口表面与应用生命周期仍需分别接入。
