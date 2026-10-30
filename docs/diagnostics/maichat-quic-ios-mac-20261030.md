# iOS 与 Mac 账号请求的 HTTP/3 接入

## 现场与范围

- 2026-10-30，Mac 创作中心请求超时、扫码登录无法生成二维码。账号域名的系统 DNS 在现场超时；同域名用备用 DNS 的 HTTP/1.1 连接可用。相关登录线路排障另见 `maichat-session-sync-dns-fallback-20261030.md`。
- 基线提交 `9e4a864a3cc1`，本记录对应工作区未提交的 QUIC 接入。修复提交和最终可安装版本待后续冻结时补充。
- 同一网络中，HTTP/3 直连账号域名超时；通过独立 curl 直连另一已知 HTTP/3 服务成功。此结果不能单独区分账号服务未启用 HTTP/3 与目标路径上的 UDP 过滤。QUIC 不能替代 DNS 解析。
- 此次为 iOS 和 Mac 引入 HTTP/3；Windows、Linux 等继续使用原 curl 8.11.1。现用 App 没有被本次隔离验收替换。

## 实现

- Apple 平台采用 curl 8.19.0、ngtcp2 1.25.0、nghttp3 1.18.0，使用已有 OpenSSL 3.5.9。两个新依赖的来源、SHA-256 和 MIT 许可证记录在各自 `MAI_UPSTREAM.md` 与 `COPYING` 中，构建从仓库源码离线完成。
- 账号传输在直连 HTTPS 路线上尝试 HTTP/3，连接失败时由 curl 降级到较早的 HTTP 版本；系统 HTTP/SOCKS 代理路线保持 HTTP/1.1。账号写请求只执行一次，不在应用层重放。
- curl 在新 Apple SDK 下误检测到仅 iOS 27 及以上可用的 `pipe2`；项目最低 iOS 16，因此 Apple 构建采用上游 `pipe` 和 `fcntl` 兜底。
- iOS 静态归档按构建开关显式加入三份 QUIC 库，关闭后不会合入旧缓存；App 内包含 curl、ngtcp2 和 nghttp3 许可证。

## 验证

- 隔离 Mac 构建运行 MaiAgent 的 8 套单元测试，8/8 通过；其中包含内嵌 curl、HTTP 下载和上传。二进制动态依赖检查未发现 Homebrew 动态库。
- 隔离 Mac 登录测试 4/4 通过：真实无凭据账号健康检查、真实二维码创建并取消、模拟二维码超时不重复请求及密码登录模拟。未操作用户账号。
- 隔离 iOS arm64 设备目标 `maichat_mobile` 编译通过；模拟器进程回读 curl 8.19.0、OpenSSL 3.5.9、HTTP3 enabled，并使用受信证书向公开服务建立 HTTP/3 连接。
- 隔离 iOS 真机 App 构建为 2.2.4 (144)，路径 `/private/tmp/maichat-quic-ios-app-derived/Build/Products/Debug-iphoneos/MaiChat.app`，签名深度校验通过。Debug 模式的 `MaiChat.debug.dylib` SHA-256 为 `5ef9e30116d4e6d8457428a45955cc31c716e162bbc7bd2fd994dbe2c3251dc5`，包含 curl 8.19.0 和 ngtcp2/nghttp3 符号；三份许可文件与源文件逐字节一致。该候选构建号与现有 144 相同，需调整后重新构建，未安装到手机。
- iOS 模拟器直接运行账号传输入口：本机 TLS 服务分别收到 HTTP/3 成功 POST、HTTP/1.1 降级 POST、收到后断开的 HTTP/3 POST 各一次；不受信证书的 POST 零次。测试服务见 `MaiChat/ios/MaiChat/scripts/test-auth-quic-fixture.py`，仅接收合成测试内容。
- 同一 Mac 构建目录中切换 QUIC ON→OFF→ON：OFF 时 HTTP3 特性消失，重启后恢复。
- iOS Debug 与 Release 的头文件搜索路径均指向 curl 8.19 的 `curl-apple/include`。在隔离 Mac 增量构建中，分别更新 ngtcp2 源文件和 QUIC 构建脚本的时间戳，均自动触发重新配置；源码变更还触发 `ngtcp2_pkt.c` 重新编译和静态库更新，内嵌 curl 测试随后通过。测试后恢复原时间戳。

## 未验证边界

- 本记录中的模拟器连接与本地服务测试不能代替真机网络和完整 App 测试。账号服务现场 HTTP/3 不可用时仍依赖已验证的 DNS 路线与 HTTP/1.1 降级。
