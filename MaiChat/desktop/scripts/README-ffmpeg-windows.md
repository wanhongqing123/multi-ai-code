# Windows FFmpeg 构建

MaiChat Desktop 的通用 FFmpeg 库从 `MaiAgent/third_party/ffmpeg` 的官方 9.0.2 源码
构建。腾讯 TRTC 自带的 `txffmpeg.dll` 仍只供 TRTC 使用；两套库互不替代。

构建机需要 MSVC 2019 x64 开发环境、LLVM 的 `clang-cl.exe`/`lld-link.exe`、
Meson/Ninja，以及 MSYS2 的 `bash`、`make`、`diffutils`、`pkgconf`。MSYS2 安装后运行：

```sh
pacman -S --needed make diffutils pkgconf
```

从 MSVC x64 开发命令行配置 MaiChat Desktop，CMake 会先用
`build-media-codecs-windows.ps1` 从仓库内源码编译 dav1d、x264、LAME 的静态库，
并使用 OpenCV 已入仓的 zlib 源码构建 PNG 所需的压缩库，
再用 `build-ffmpeg-windows.ps1` 构建 FFmpeg。MSYS2、Meson 不在标准目录时可传入
`-DMAICHAT_MSYS2_BASH=C:/path/to/msys64/usr/bin/bash.exe` 和
`-DMAICHAT_MESON=C:/path/to/meson.exe`。这些库安装到构建目录的
`vendor/media-codecs-windows/install`，FFmpeg 的七个静态库、进程内命令库与公开头
写入 `vendor/ffmpeg-windows`；
`maichat_image_filter` 使用同一份 `MaiChat/MaiChatTools/MaiImageFilter.cpp`，并由
`desktop_ffmpeg_filter_test` 验证灰度和旋转的实际执行。

构建关闭自动探测、非自由组件与汇编，开启 GPL、libdav1d、libx264、libmp3lame、zlib，
使用动态 CRT (`/MD`) 以匹配 Qt 桌面端。FFmpeg 的实际 AV1 抽帧、滤镜加 x264
重编码、LAME MP3 输出与 FFprobe 回读由 `MaiEmbeddedFfmpegToolsTest` 验证。
因为 x264 使产物进入 GPL 配置，分发时须按源码目录的许可证履行义务。
Desktop CMake 会修正个别中文 VS2019 环境下被双重解码的 `/showIncludes` 前缀；
否则 Ninja 可能漏记头文件依赖，拉取新接口后仍链接旧对象。
原生 FFmpeg Windows 构建要求 MSYS2，详见
[FFmpeg 平台文档](https://ffmpeg.org/platform.html#Microsoft-Visual-C_002b_002b-or-Intel-C_002b_002b-Compiler-for-Windows)。
