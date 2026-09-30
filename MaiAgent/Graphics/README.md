# MaiAgent Graphics

这里移植了 OBS 的 Graphics、util、Metal 和 OpenGL 源码。版本、目录映射、许可
与平台适配见 [`UPSTREAM.md`](UPSTREAM.md)。对外接口是 OBS 原有的 `gs_*`，
入口在 [`../include/MaiGraphics.h`](../include/MaiGraphics.h)。应用图片呈现入口
是 [`../include/MaiGraphicsPresenter.h`](../include/MaiGraphicsPresenter.h)：MaiChat
提供原生渲染视图与打包的 effect 目录；后台解码线程用 FFmpeg 解码图片，
唯一的 Graphics 线程通过 `default.effect` 绘制到交换链并直接呈现，不回读像素。
视图绑定、图片切换和尺寸变化都是异步命令。
`MaiGraphicsPresenterShowFrame` 可提交已解码的 RGBA 视频帧；
[`../include/MaiVideoPlayback.h`](../include/MaiVideoPlayback.h) 提供 FFmpeg
视频读流、解码、播放、暂停、跳转、逐帧、倍速和循环的第一阶段接口。
当前它**没有音频输出**，也尚未替换 MaiChat 的有声视频播放器。

启用 `MAIAGENT_BUILD_GRAPHICS=ON` 后：

- macOS Apple Silicon 构建 `MaiAgent::Graphics` 动态库及 Metal 模块。
  消息图由 MaiChat 的原生子视图提供 `CAMetalLayer`；真实 NSView 直显、
  离屏清屏和 effect 加载测试已通过。MaiChat.app 打包两项库及 effect 资源。
- iOS 静态链接 Metal，使用 `gs_create(..., "builtin:metal", ...)`。
  模拟器实测 CAMetalLayer 直显入口；消息图、封面和 AI 图片视图已接入。
  真机 SDK 交叉编译与 MaiChat Workspace 构建通过。
- Android 构建 Graphics 和 GLES 两个 `.so`。GLES shader 使用 3.0 语法；
  arm64 GLES 3.1 模拟器实测 `default.effect` 和 `TextureView` 直显的红色像素。Debug APK 含
  两种 ABI 的库及 effect 资源；聊天图、视频封面和 AI 图片视图已接入。
- Windows 应使用 D3D11；运行时尚未接通。macOS、iOS 和 Windows 均不使用
  OpenGL。Linux 的 X11/Wayland OpenGL 实现源码保留，但当前未进入 MaiAgent
  构建。桌面 Qt 图片加载逻辑共用，后端由平台选择。

OBS `libobs/data` 的 21 个 `.effect` 文件位于 `data/` 并进入三端资源包。
macOS Metal 已验证 `default.effect` 和 `solid.effect`；目前有 9 个矩形纹理
或去隔行 effect 在 Metal 上不能编译，不能视为全部可用。

Mac 构建的 FFmpeg 9.0.2 启用 VideoToolbox 和 AudioToolbox。独立 Graphics
构建仍用 `compat/` 隔离 OBS 应用层信号，视频信息固定为 SDR，尚未转发
`video_reset`。三端媒体图片视图使用共用 Presenter；渲染失败时显示失败状态，
不再切换到系统图片解码。macOS、iOS 已验证原生 Layer 入口，Android 已做 View 像素验收；
真实设备上的滚动、视图复用和色彩仍需验收。视频帧已能通过共用 Presenter
呈现并在 macOS、iOS、Android 的测试中执行；音频输出、音画同步和现有
播放器 UI 的切换仍未完成。
