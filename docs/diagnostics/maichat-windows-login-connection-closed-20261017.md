# MaiChat Windows 扫码登录显示 `Connection closed`

## 现场与复现

- 时间：2026-10-17 00:52–00:59 CST。
- 平台：Windows x64，MaiChat Desktop 0.1.97；运行路径为
  `D:\Program Files\MaiChat\maichat.exe`，SHA-256 为
  `ed9e6cd4ce20cbe769742be534f4d5b91a07d94c9f154d9136f6c388b249a121`，
  与发布包内程序一致。源码及发布标签为
  `3101cfabab7b46a3b43b755bd432869d54a8842b`。
- 用户操作：打开安装版登录页并尝试生成二维码或重试连接。
- 预期：收到二维码，或显示服务端返回的明确错误。
- 实际：二维码为空，界面显示“网络连接失败：Connection closed”。

## 定点证据

`DesktopAuthClient` 的正式入口是 `https://ichat.life/api/v1/`，二维码请求为
`POST auth/desktop/request`。它使用 Qt `QNetworkAccessManager`；网络错误先于
HTTP 状态码出现，`responseError` 将 `QNetworkReply::errorString()` 显示给用户。
应用日志确认 Qt 支持 SSL，加载的 OpenSSL 版本为 1.1.1g；安装目录内也有对应的
`libssl-1_1-x64.dll` 和 `libcrypto-1_1-x64.dll`。

同一台 Windows、同一 Wi-Fi 和同一 DNS 地址 `8.134.253.45` 的无凭据对照：

| 路径 | 结果 |
|---|---|
| TCP 443 | 建连成功。 |
| Windows curl / Schannel，强制绕过代理访问正式 HTTPS 地址 | TLS 握手收到 reset，未得到 HTTP 响应。 |
| Git curl / Schannel，强制绕过代理 | 同样在 TLS 握手收到 reset。 |
| MSYS2 curl 8.20.0 / OpenSSL 3.6.3，强制绕过代理，对 `/api/v1/auth/me` 发无凭据 GET | TLS 1.3 握手、证书链与 `ichat.life` 主机名验证成功；服务返回预期 HTTP 401 JSON。 |
| 另一份 curl 8.18.0 / OpenSSL 3.5.4，强制绕过代理，发相同 GET | 返回预期 HTTP 401 JSON。 |
| 明文 HTTP，同域名任一路径 | 返回 `403 Server: Beaver`，页面标题为 `Non-compliance ICP Filing`。 |

明文端口的 403 页面不能证明 HTTPS 失败由备案状态导致，因为同机 OpenSSL 客户端能
以有效证书访问正式账号服务。相同域名此前在 Android 蜂窝网络也发生过与 TLS
握手特征有关的重置，并有服务端抓包证据；详见
[Android 排障记录](maichat-android-sms-connection-reset-20261016.md)。
当前 Windows 证据只证明不同 TLS 客户端的结果不同，尚不能确定重置由哪段链路
或哪种 ClientHello 字段触发，也尚未抓取 Windows 请求的服务端报文。

## 当前状态与验证边界

Mac/服务端随后抓包确认：同出口的失败系统 TLS 客户端在 HTTP 请求前使连接复位，
紧邻的 OpenSSL 请求完成证书校验并得到 HTTP 401。公网 OpenSSL 默认 TLS 1.3
可达、强制 TLS 1.2 失败；服务器本机 TLS 1.2 又可握手。单侧抓包仍不足以给
RST 的发送设备归属，不能简化成服务器禁用 TLS 1.2 或仅由 SNI 触发。

用户明确要求 Windows 和 macOS 桌面端复用 MaiAgent 自编译 curl + OpenSSL。
Mac 已提交共享桌面账号传输 `a26e579b`，保留证书及域名校验、禁重定向和
POST 自动重放。Windows 旧 MaiAgent curl 后端为 Schannel；新构建以 OpenSSL
3.5.9 静态 SSL+Crypto 替换后，`MaiCurlEmbeddedCliTests` 从后端断言失败转为
通过。在同一受影响 Windows 网络，用这份库、Windows 原生 CA 和完整 TLS
校验，对正式 `/api/v1/auth/me` 发无凭据 GET 得到 HTTP 401。桌面产品重新链接
后，`LoginDialogTest` 的同一正式 GET 探针也通过。

首次执行正式 `POST /api/v1/auth/desktop/request` 的 opt-in 二维码探针时，
得到 HTTP 404，未重复创建请求。`services/auth/service.js` 的对应路由于
2026-10-16 的 `58d5f4f5` 才新增；服务端协作者确认线上 systemd 仍运行
2026-10-15 的旧 `service.js`，该文件没有 desktop request/claim/cancel 路由，
Nginx `/api/v1/` 转发路径正确，直连后端的新路由也返回 404。这是 TLS
问题后面的第二个独立阻断。

服务端随后在 SQLite 在线备份和 38/38 单测通过后部署新路由；无效请求的
直连和公网返回均从 404 变为预期 400。Windows 在部署完成后**只执行一次**
正式二维码创建、PNG 加载与取消测试，`LoginDialogTest` 得到 22 通过、
0 失败、1 条未启用的只读探针跳过。服务端回读显示
`desktop_login_requests` 仅有 1 条且状态为 `canceled`，原有 6 条会话不变，
生产 SQLite `integrity_check=ok`。同一 Windows 构建的完整桌面 CTest
为 61/61；真正由用户手机扫码批准和后续 IM 凭据获取尚未验证，不能据此宣称
整条登录路径已经端到端通过。

公开的 0.1.97 Release 保持原资产不覆盖。用户指定 MaiChat Desktop
下一版为 2.1.0，应从同一最终提交重建并另发独立 Release。

独立 MaiAgent Windows CTest 为 37/38：`MaiTurnRunnerTests` 的专家任务自动
通知与输出路径断言失败，单独复跑仍失败；该测试不调用本次账号传输，失败未
归因于 TLS 改动。桌面产品 61/61、嵌入式 curl 后端红绿对照和正式账号探针
是本次修复的直接验证；不能把 MaiAgent 全套称为通过。
