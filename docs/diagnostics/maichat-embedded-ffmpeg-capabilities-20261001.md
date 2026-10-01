# MaiChat 内置 FFmpeg 编解码能力与错误诊断（2026-10-01）

## 现场与复现

- 用户在 macOS 上报告：进程内 `ffmpeg` 可做流复制和 H.264 抽帧，但
  `libx264`、`libmp3lame`、AV1、`lavfi`、`h264_videotoolbox` 和部分滤镜链失败；
  Agent 只收到数字状态码。
- 当时源码基线为 `b0d45864`。实际运行 App 的完整路径、原始 AV1 样本及
  原始工具参数未提供，因此不能把该用户机器上每一项失败都归于同一原因。

## 定点证据

- 原 Mac 内置构建由 `MaiAgent/Graphics/scripts/build-ffmpeg-runtime.sh` 配置，
  使用 `--disable-autodetect --disable-gpl --disable-nonfree`；VideoToolbox 明确启用。
  原 `config_components.h` 显示 `LIBX264_ENCODER=0`、`LIBMP3LAME_ENCODER=0`、
  `LIBDAV1D_DECODER=0`，而 `H264_VIDEOTOOLBOX_ENCODER=1`、`LAVFI_INDEV=1`。
- `eq` 和 `hqdn3d` 在 FFmpeg 的 `configure` 中依赖 GPL；内置构建的
  `EQ_FILTER=0`、`HQDN3D_FILTER=0` 是这一配置的结果。
- `AV1_DECODER=1` 不能证明有软件解码。该版本的 `libavcodec/av1dec.c`
  明确写着没有原生软件解码；无可用硬解时返回 `ENOSYS`。本机 64×64 AV1
  样本的进程内抽帧实测返回状态 69，并输出“Your platform doesn't support
  hardware accelerated AV1 decoding”。这与用户报告的 AV1 失败一致，但不能
  代替对用户原文件的验证。
- 用同一进程内工具在本机成功完成 `lavfi testsrc` → PPM，以及
  `h264_videotoolbox` → MP4，再用内置 `ffprobe` 确认 H.264 输出。
  因此这两项不是构建层面普遍缺失；用户具体失败仍需原参数和日志定位。
- 工具层此前只拼接状态数字，FFmpeg 的 `av_log` 错误留在进程 stderr，
  Agent 看不到具体原因。

## 修复与验证

- 编解码器与错误回传修复提交：`e141f035bd8b3f029074e15a15bb485a75c2b126`。
- 进程内 FFmpeg/FFprobe 工具调用期间收集最多 4 KiB 的警告与错误日志，
  并用 `av_strerror` 翻译返回码；失败时把两者返回给 Agent。日志回调在
  FFmpeg 工作线程上可能并发，因此收集缓冲加锁；调用结束恢复默认回调。
- 根据用户后续明确要求，仓库固定了 `dav1d 1.5.4`、VideoLAN x264 stable
  提交和 `LAME 3.101` 的完整源码；共享脚本先构建静态库，再以
  `--enable-libdav1d --enable-libx264 --enable-libmp3lame --enable-gpl` 配置 FFmpeg。
- macOS 的进程内工具测试通过 AV1 软件抽帧、`lavfi testsrc`、
  `hqdn3d + eq + unsharp + libx264` 转码、LAME MP3、VideoToolbox H.264
  编码与失败诊断；MaiAgent 23/23 测试及播放器/Graphics 定向测试通过。
- Android arm64 的同份静态库在 Pixel 9 模拟器实际运行三项转码，回读文件为
  PPM、H.264、MP3；Debug APK 两个 ABI 构建通过。iOS arm64 模拟器的
  进程内测试同样完成三项转码，MaiChat.app 编译、安装和启动通过。

## 路由与未验证边界

- MaiChat 的 Agent 及移动端保持进程内内置引擎，避免隐式依赖用户机器上
  不同版本的 FFmpeg。Desktop 若需调用 Homebrew FFmpeg，应由用户明确选择
  外部工具；不根据数字失败码自动回退到 shell，也不在移动端启动子进程。
- 新配置包含 GPL 组件，发布二进制时必须按 FFmpeg/x264/OBS 等各组件的
  许可证要求提供相应源码、构建材料与声明；不能沿用旧 LGPL-only 说明。
- 未拿到用户原始 AV1 文件与失败参数，不能确认其具体 profile、滤镜组合或
  VideoToolbox 会话失败的独立原因。Windows 构建和实际转码由 Windows
  工作区继续验证；iOS 真机及 Android 实机的性能、发热、内存尚未实测。
