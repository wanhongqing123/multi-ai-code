# Windows FFmpeg 构建

MaiChat Desktop 的通用 FFmpeg 库从 `MaiAgent/third_party/ffmpeg` 的官方 9.0.2 源码
构建。腾讯 TRTC 自带的 `txffmpeg.dll` 仍只供 TRTC 使用；两套库互不替代。

构建机需要 MSVC 2019 x64 开发环境、LLVM 的 `clang-cl.exe`/`lld-link.exe`，以及
MSYS2 的 `bash`、`make`、`diffutils`、`pkgconf`。MSYS2 安装后运行：

```sh
pacman -S --needed make diffutils pkgconf
```

从 MSVC x64 开发命令行配置 MaiChat Desktop，CMake 会自动执行
`build-ffmpeg-windows.ps1`。MSYS2 不在标准目录时可传入
`-DMAICHAT_MSYS2_BASH=C:/path/to/msys64/usr/bin/bash.exe`。
七个静态库与公开头写入桌面 CMake 构建目录的 `vendor/ffmpeg-windows`；
`maichat_image_filter` 使用同一份 `MaiChat/shared/agent/MaiImageFilter.cpp`，并由
`desktop_ffmpeg_filter_test` 验证灰度和旋转的实际执行。

构建关闭自动探测、GPL/非自由组件与汇编，使用动态 CRT (`/MD`) 以匹配 Qt 桌面端。
原生 FFmpeg Windows 构建要求 MSYS2，详见
[FFmpeg 平台文档](https://ffmpeg.org/platform.html#Microsoft-Visual-C_002b_002b-or-Intel-C_002b_002b-Compiler-for-Windows)。
