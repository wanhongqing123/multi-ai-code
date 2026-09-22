# Mbed TLS for Android

Android NDK 不提供公共的 OpenSSL 开发库，因此 Android 的 MaiAgent/curl 使用 Mbed TLS；iOS 继续使用系统 Security，桌面端保持原有后端。

- 版本：Mbed TLS 3.6.7（LTS）
- 官方发布：https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7
- 原始完整源码包：https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
- SHA-256：`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`
- 与 GitHub Release asset digest 核对一致。构建时再次校验，再解压到构建目录；不在配置阶段联网下载。
- 许可证与版权文件完整保留在源码包内；本目录另外放置许可证便于查看。
- 启用 pthread 线程支持。Android 的根证书由系统 TrustManager 导出，只保存公开证书；模型客户端和 webfetch 都使用相同信任库，保持主机名及证书校验。
