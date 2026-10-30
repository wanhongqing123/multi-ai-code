# 会话同步 WSS 账号域名备用线路（2026-10-30）

## 现象与范围

用户反馈 MaiChat 的账号域名在本机网络异常时无法使用，要求登录、配置和其他账号服务具备备用线路。此次检查会话同步：用户没有报告或提供证据表明本次故障一定发生在 WSS，不能将账号 DNS 故障直接归因到 WSS。源码基线 `9348dccd0e720472fc3d05787f25945726caf7fe`，修改尚未提交。

旧实现以 websocketpp/ASIO 连接账号 WSS。iOS 使用创建时的系统代理；Mac 每次重建连接时重新读取代理。两端使用域名解析或 HTTP CONNECT，缺少备用 DNS；发生连接错误时未完成操作会失败，不自动重放。TCP WebSocket 不使用 curl QUIC。

## 修改

- 仅账号域名 `ichat.life:443` 的 WSS 系统线路失败时，依次使用阿里和 Cloudflare DoH 进行无凭据 HTTPS 探测，确认账号接口返回 401 后使用探测出的 IP 建立 TCP 连接。其他主机维持原线路。
- 连接地址与 URL 分离：WebSocket Host、TLS SNI、证书链及域名验证继续使用 `ichat.life`。代理不可用时允许该域名直连；HTTP 拒绝登录时不会切换备用路线。断线不重放写操作。
- DoH 探测各设 4 秒总超时；当前同步线程不能在一次探测中途取消。每次断线可重新探测，不固化服务端 IP。

## 验证

- `python3 MaiChat/SessionSync/tests/run_routed_socket_test.py`：本地 TLS fixture 将系统代理指向不可用地址，测试编译专用注入备用地址。确认 SNI 和 Host 保留 `ichat.life`；错误域名证书被拒，服务端未收到授权请求；断线重连后只收到一次写帧。测试注入仅存在于 `MAICHAT_SESSION_SYNC_TEST` 构建。
- 独立审查发现首个备用 IP 连接失败后，原代码继续重复该地址而未尝试第二条路线。首路注入不可连接的 `127.0.0.2`、次路注入 `127.0.0.1` 的回归在修复前以退出码 5 失败，清空已失败地址后通过；两条路线均失败后按原有 2 秒间隔重试。
- `cmake --build MaiChat/desktop/build-release-v2.2.31 --target desktop_session_sync_channel_test -j 3` 后执行 `ctest --test-dir MaiChat/desktop/build-release-v2.2.31 -R '^desktop_session_sync_channel_test$' --output-on-failure`：1/1 通过。
- 指定 arm64 模拟器 `34893056-B994-4CFE-96C2-5526174AC370` 的 iOS Debug App 编译成功；使用 generic 模拟器时因包含未支持的 x86_64 导致 OpenSSL 构建失败，不作为功能验证。
- 本机独立 curl、无代理地以阿里 DoH 访问生产账号的无凭据 `auth/me`，取得 HTTP 401；该检查没有连接 WSS 或使用账号令牌。
- 另以独立进程直接调用 WSS 的 `probeAddress`，没有预先启动账号或模型网络模块，使用 App 的 CA 包和阿里 DoH 探测成功；模块内部已负责初始化 curl。

## 未验证边界

本地 WSS 测试对备用地址使用测试注入，不能证明生产网络 DoH 与 WSS 全链路；没有对 Android、真机、生产授权 WSS、认证代理或 IPv6 实网执行验收。需要最终构建后由执行者继续做目标平台验证。
