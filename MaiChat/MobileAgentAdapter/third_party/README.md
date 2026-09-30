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
- `scripts/build-ffmpeg.sh` 为 iOS、Android ABI 和本机测试构建 FFmpeg 默认的全部内置库、软件编解码器、格式、协议及滤镜。移动 App 使用库 API，因此不打包命令行程序和文档；关闭外部自动检测、汇编优化与 Apple 专用硬件后端，以保持移动端共享库可链接。不启用 GPL 或 nonfree 组件。当前图片文件的读写仍使用系统 API，FFmpeg 负责 Agent 的滤镜操作。
- FFmpeg 源码目录包含 `LICENSE.md` 和 LGPL 文本。应用分发时还需按 [FFmpeg 官方授权说明](https://ffmpeg.org/legal.html)提供相应的许可声明、源码及可重新链接的构建材料。
