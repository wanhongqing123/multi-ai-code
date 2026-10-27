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

完成了 Windows Desktop 2.2.30 的源码修复候选：先对系统代理线路发送无凭据
`GET /auth/me`，它失败时再对直连发送同一个只读探针；只有真实拿到预期 401
才选该线路。若两个短探针都没结果，保留原代理供正式请求使用，避免误伤较慢
但必需的代理。选中的线路同时供账号 HTTP 和 Session WSS 使用；代理设置变化
或账号请求发生传输错误后重新探测。真正的登录 POST 只发送一次，绝不因
超时自动重放。Mac 和 iOS 的代理实现没有改动。

同一 Windows VS2019/Qt Release 构建下，路由单测先因“两路短探针均失败时
误判不可用”失败，改为保留原代理后通过。原先带机器代理的现网无凭据账号
GET 约 10 秒超时；修复候选在不修改代理环境的情况下通过，耗时 2.685 秒。
正式二维码创建、PNG 验证与立即取消也通过，耗时 2.874 秒。选路、登录、
Session WSS、Session Host 与 AgentController 五组完整 CTest 5/5 通过。
实际只能通过代理的外部网络和 Mac 构建尚未实测，不能把本机验证扩大为这两项。

产品与打包修复提交为 `7b788479f589153893330b4e34148543826fe02d`。
从该精确提交增量重链 Windows VS2019/Qt Release 后，路由、登录、
Session WSS、Session Host、AgentController 五组 CTest 再次 5/5 通过；
带原机器代理环境的正式无凭据 GET 和二维码创建后取消均通过。源码工作树干净。

排障时还出现独立的测试流程错误：从 `build/bin/maichat.exe` 裸启动暂存
`--smoke`，因该目录未旁挂 Qt5Sql.dll 弹出系统缺 DLL 窗口。该进程由本次
测试启动并已终止；用户安装目录中的 Qt5Sql.dll 存在，安装版 exe 未被
覆盖。历史 `package-windows.ps1` 已有 `windeployqt` 组装流程；本次在
脚本中增加 Qt 核心 DLL、Qt5Sql 和插件缺失即报错检查，并在 README 明确
烟测只能运行完整 staging。独立暂存目录从 2.2.30 Release 构建生成，
在仅含 Windows 系统目录的 PATH 下实际 `--smoke` 退出 0；日志回读运行路径、
版本 2.2.30 和 OpenSSL 3.5.9。该暂存版尚未安装到用户机器的现用目录。

独立 Windows 资产来自 `7b788479`，尚未上传或发布：安装器
`MaiChat-Setup-win64-v2.2.30-7b788479.exe` 为 112,897,129 B，SHA-256
`bb694c2e1ac0d8a3abbdef06aed2e2308f7ae03779c93e8bb104ef192ead49d0`，
当前无 Authenticode 签名；ZIP
`MaiChat-win64-v2.2.30-20261027-7b788479.zip` 为 139,882,958 B，
SHA-256 `afd105559b05dbc7f67661ff0f494783a91b697782c36b44994cccd486241b55`。
ZIP 中的 `maichat.exe` SHA-256 与 staging 一致，为
`2d216e880d8814fdfedadd726117eaac43ff1bb0393847e26296b596ef68c3ce`。
实际从 ZIP 解压到全新隔离目录后，核对 Qt5Sql.dll、qwindows.dll、
qsqlite.dll 和 exe 均在，再以裸 PATH 运行解压出的程序，`--smoke` 退出 0。
用户已安装 `D:\Program Files\MaiChat\maichat.exe` 仍是 2.2.29，SHA-256
`3a1c25a73e7510ae1e35366854d9e673d03754748bc6a09277145c816ca37a70`；
本轮未运行安装器、未覆盖该程序。
