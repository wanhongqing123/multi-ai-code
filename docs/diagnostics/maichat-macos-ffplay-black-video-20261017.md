# MaiChat macOS 视频有声音但黑屏

- 时间与现场：2026-10-17，macOS 26.4、Apple M3 Pro，Desktop 2.1.2。用户在 ffplay 视频弹窗播放约 5 秒的本地 MP4，音频正常、画面区域全黑。接手时 `/Applications/MaiChat.app/Contents/MacOS/MaiChat` 正在运行；源码基线为 `a1d00e3a3a92d719bd9d92e7f7963504fd6eb1f2`。
- 复现步骤：打开 `/Users/hongqingwan/Movies/wan3-6827a0d6.mp4` 或 `/Users/hongqingwan/Movies/kling-aa2d80f7.mp4`；等待播放约 1 秒；观察视频区域。两者均为 H.264 1080p，`ffmpeg` 提取的首帧和 1 秒帧有正常图像。修复前，测试窗口裁剪截图的视频区域像素范围为 0–0，内置 64×64 H.264 测试视频也呈黑色。
- 根因证据：`MaiGraphicsPresenter` 的视频帧及注入的 RGBA 纯色帧均回报渲染成功，但 macOS Metal 后端 `MetalDevice.setupDisplayLink()` 只设置回调，没有启动 `CVDisplayLink`，而 `device_present()` 在 macOS 不执行 `blitSwapChains()`。启动显示定时器后，上屏拷贝开始执行，单独这一处仍被 Qt 视频控件的背景绘制遮住。将该控件设为 `WA_PaintOnScreen` 与 `WA_NoSystemBackground` 后，同一测试画面由全黑变为可见；此前测试用 `grabWindow(surface->winId())` 抓到整个桌面，把背景像素误判为视频画面。
- 修复提交：`84c27ad4b7a8e8836c429d5e42b2850c878a5737`。macOS Metal 初始化后启动 `CVDisplayLink`；Mac 视频控件停止 Qt 背景重绘；测试改为按控件的屏幕矩形抓图并检查像素变化。给 Desktop 视频弹窗测试补齐 Metal 后端和视频转换 effect 的构建依赖。
- 红绿验证：修复前 Wan 视频区域 0–0，修复后 0–255；Kling 视频修复后 0–255。已目视检查两份修复后截图，均有实际画面。`MaiFfplayVideoDialogTest`、`MaiFfplayHostMacTests`、`MaiGraphicsPresenterMacTests` 3/3 通过；弹窗测试包含打开、暂停、恢复、拖动、倍速和关闭。修复只在 macOS 分支生效，Windows/iOS/Android 代码路径未改。
- 交付边界：测试在当前源码构建的 Mac Qt 弹窗上完成；2.1.2 Mac 可移植 App 已打包并通过 `codesign --verify --deep --strict`，路径为 `MaiChat/desktop/dist-video-black-fix/MaiChat-macos-arm64/MaiChat.app`。用户仍在运行的 `/Applications/MaiChat.app` 未被停用或替换，因此原进程仍是旧代码。Windows/iOS/Android 的实机媒体播放未因本次修复重新验收。
