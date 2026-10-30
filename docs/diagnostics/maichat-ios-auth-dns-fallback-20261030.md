# iOS 账号请求 DNS 备用线路

- 现场：2026-10-30，用户报告 Mac 二维码登录一直加载，要求账号、凭据等域名请求具备多条备用线路。iOS 真机当前未连接，本项未在真机复现或验收。
- 修改基线：iOS `MaiChatAuthTransport.cpp` 原来使用 libcurl、系统代理和系统 DNS；DNS 失败时没有备用解析，HTTP 写入请求不自动重试。iOS 账号登录、IM 凭据、模型密钥、ASR 临时凭据共用此传输。
- 处理：对生产账号域名先用无凭据的 `auth/me` 探测系统线路；失败时并发探测阿里与 Cloudflare DoH 直连，选成功的线路发送一次实际业务请求。60 秒内按代理配置缓存线路；传输失败失效，后续请求重新选择。证书、SNI 和业务 URL 保持原域名，禁跳转。POST 超时可能已经被服务端处理，绝不自动重放。
- 模拟器构建：iOS Debug 模拟器构建成功，构建日志 `/private/tmp/maichat-ios-auth-route-build.log`。测试代码只在 `MAICHAT_AUTH_ROUTE_TEST` 编译时注入；正式 App 不包含注入入口。
- 定向测试：在仓库根目录执行下列命令。第一个测试注入探测和发送结果，覆盖系统线路失败、代理配置不可解析、缓存及 POST 超时只发送一次；第二个使用本机 DoH 和 HTTPS 服务，验证受信任证书可用、错误 CA 和错误主机名被拒绝、健康探测无账号凭据。测试在 Mac 主机编译同一份传输源码，链接主机 libcurl，不能替代 iOS 真机验收。

```sh
xcrun clang++ -std=c++17 -pthread -I MaiAgent/third_party/curl/include MaiChat/ios/MaiChat/scripts/test-auth-route.cpp -framework CFNetwork -framework CoreFoundation -lcurl -o /private/tmp/maichat-auth-route-test
/private/tmp/maichat-auth-route-test
python3 MaiChat/ios/MaiChat/scripts/test-auth-route-tls.py /private/tmp/maichat-auth-route-test
```

- 现场网络观察：本机无代理的系统 DNS 请求超时；主机 curl 使用 `https://223.5.5.5/dns-query`、`https://1.1.1.1/dns-query` 分别直连生产 `auth/me` 均收到 401，启用了 HTTPS 证书和主机名验证，未发送凭据。不能据此推定任意网络或设备均可用。
- 范围：云盘、视频圈、会话同步 WSS、模型供应商网络等不经过此账号传输，尚未获得本次备用线路。HTTP/3 / QUIC 尚未启用，不能代替 DNS 解析。
- 未验证：真机安装、真机登录，以及 iOS 随包 libcurl 的实际 DoH 网络和证书行为。
