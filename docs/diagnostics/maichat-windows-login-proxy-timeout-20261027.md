# Windows 扫码登录经本机代理超时（2026-10-27）

## 现场与预期

- 时间：2026-10-27 22:02–22:08 CST；Windows 11 25H2，build 26200.9457。
- 用户运行 `D:\Program Files\MaiChat\maichat.exe`，日志回读版本 2.2.29、
  Qt 5.15.2、内嵌 libcurl TLS 后端 OpenSSL 3.5.9；运行文件 SHA-256 为
  `3a1c25a73e7510ae1e35366854d9e673d03754748bc6a09277145c816ca37a70`。
  当前仓库 HEAD 为 `c5aee885fe57d45c1ed9f5da160099f3fdde97fa`；未用
  当前 HEAD 冒充已安装二进制的精确源码提交。
- 操作：打开扫码登录，等待生成二维码。预期取得可扫码二维码；实际显示
  “网络连接失败：Timeout was reached”，没有二维码。

## 定点证据

- 登录 UI 的 `LoginDialog::continueQrRequest` 调用
  `DesktopAuthClient::createLoginRequest`；底层
  `DesktopCurlAuthTransport::perform` 使用 libcurl 请求
  `https://ichat.life/api/v1/auth/desktop/request`。该错误来自 curl 的超时，
  不是 PNG 加载失败。
- 本机 DNS 将 `ichat.life` 解析到 `8.134.253.45`，443/TCP 建连成功。
- Windows 机器级 `https_proxy` 和当前进程的 `https_proxy` 均指向
  `127.0.0.1:10808`；该端口由 v2rayN 的 `sing-box.exe` 监听。
- 同一 VS2019/Qt Release 的 `LoginDialogTest::liveProductionAuthHealthCheck`：
  保留环境代理时，正式无凭据 GET 在约 10 秒后报 `Timeout was reached`；
  只在测试进程中移除 `https_proxy`、`http_proxy` 后，58 毫秒内收到预期
  HTTP 401，测试通过。
- 独立 OpenSSL curl 保留环境代理时 5 秒超时；`--noproxy '*'` 直连时
  TLS 握手和证书校验成功，约 0.19 秒收到预期 HTTP 401。
- 使用同一个 `127.0.0.1:10808` 代理对照，`www.baidu.com` 可返回
  HTTP 200，而 `ichat.life` 约 7 秒后超时；后者直连约 0.12 秒返回
  预期 HTTP 401。这证明代理不是对所有国内域名都失效，不能由其他网站
  能打开推断 MaiChat 登录域名的代理链路正常。
- 仅在测试进程中移除代理变量后，正式
  `LoginDialogTest::liveProductionQrCreationAndCancellation` 通过，
  二维码请求创建、PNG 验证与取消合计 432 毫秒。未扫码、未登录账号。
- Windows 系统 curl/Schannel 的直连 TLS reset 是已知的另一条链路表现，
  不能拿它推翻本次产品使用的 OpenSSL 直连成功证据；参见
  `maichat-windows-login-connection-closed-20261017.md`。

## 根因与跨端边界

- `DesktopCurlAuthTransport.cpp` 于 2026-10-17 01:18 +08:00 在提交
  `a26e579bf92551131a37e4e774e26b14d3c6e527` 引入。该实现仅对本机
  HTTP 测试地址显式设置 `CURLOPT_PROXY=""` / `CURLOPT_NOPROXY="*"`；
  正式 HTTPS 没有显式代理选项，因此 libcurl 读取进程代理环境变量。
  没有后来单独新增的“继承代理”产品分支。
- Windows 的本机代理链路对该域名超时，而同机同客户端直连正常，足以解释
  本次登录失败。尚未定位 `sing-box` 内部为何超时，也未查明机器级代理变量
  何时由哪个程序设置；不能把它归因于服务端或证书。
- Windows 与 macOS Desktop 共用 `DesktopCurlAuthTransport`。macOS 的
  系统代理是否被导出为 MaiChat 进程环境变量、以及该代理是否可用，本次
  没有 Mac 进程证据，不能仅凭“Mac 开了代理”断定它走同一链路。
- iOS 账号请求使用另一份 `MaiChatAuthTransport.cpp`，经
  `MaiChatSystemProxy.h` 显式覆盖 curl 的代理设置；不复用 Desktop 的
  代理选择代码。

## 当前状态

仅完成只读诊断及一次可取消的正式二维码测试；未修改系统代理、产品源码或
运行中的 MaiChat，用户当前安装版仍会使用其继承的代理环境。后续修复需避免
对有副作用的登录 POST 在结果不明时自动重放，并分别验证直连网络与必须经过
代理的网络环境。
