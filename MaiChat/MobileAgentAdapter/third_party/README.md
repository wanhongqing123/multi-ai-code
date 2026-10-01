# Mbed TLS for Android

Android NDK 不提供公共的 OpenSSL 开发库，因此 Android 的 MaiAgent/curl 使用 Mbed TLS；iOS 继续使用系统 Security，桌面端保持原有后端。

- 版本：Mbed TLS 3.6.7（LTS）
- 官方发布：https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7
- 原始完整源码包：https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
- SHA-256：`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`
- 与 GitHub Release asset digest 核对一致。构建时再次校验，再解压到构建目录；不在配置阶段联网下载。
- 许可证与版权文件完整保留在源码包内；本目录另外放置许可证便于查看。
- 启用 pthread 线程支持。Android 的根证书由系统 TrustManager 导出，只保存公开证书；模型客户端和 webfetch 都使用相同信任库，保持主机名及证书校验。

# FFmpeg for mobile media tools

- 版本：FFmpeg 9.0.2；完整源码放在 [`MaiAgent/third_party/ffmpeg/`](../../../MaiAgent/third_party/ffmpeg/) ，与 Curl 同级。
- 来源：官方源码包 https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz，SHA-256 为 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`。构建直接使用仓库里的源码目录，不联网下载。
- `scripts/build-ffmpeg.sh` 为 iOS、Android ABI 和本机测试构建 FFmpeg 内置组件，先从仓库源码构建 `libdav1d`、`libx264`、`libmp3lame`，再通过各自的配置开关接入 FFmpeg；构建过程中不下载源码，也不链接开发机 Homebrew 的编解码库。宿主仍需 Meson、Ninja、Make、pkg-config 和对应平台的 C 工具链。移动 App 使用进程内库 API，不打包命令行程序。关闭外部自动检测和汇编优化；移动端不启用 Apple 专用硬件后端。
- `libdav1d` 补 FFmpeg 9.0.2 中缺失的 AV1 软件解码路径；`libx264` 提供 H.264 重编码，`libmp3lame` 提供 MP3 编码。启用 GPL 后 `eq`、`hqdn3d` 等滤镜也随默认配置构建。
- 此配置使用 `--enable-gpl`，FFmpeg 与 `libx264` 的相关产物受 GPL 约束。发布产物前须按 [FFmpeg 官方授权说明](https://ffmpeg.org/legal.html)以及各源码目录中的版权文本提供相应源码、构建材料和许可声明。不能沿用先前的 LGPL-only 分发说明。
