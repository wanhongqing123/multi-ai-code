# Android 获取验证码时的 `Connection reset`

## 现场与复现

- 时间：2026-10-16 10:36–10:55 CST。
- 设备：OP5A47，Android 15，MaiChat 0.1.79（build 79），从现有版本保留数据升级。
- 源码：`b14d5417`；真机的正式账号地址是 `https://ichat.life/api/v1/`。
- 用户操作：登录页输入手机号、勾选年龄和协议，然后点击“获取验证码”。
- 预期：显示验证码输入框，或者显示有含义的服务端错误。
- 实际：界面显示原始异常 `Connection reset`。真机日志在 10:36:55 与
  10:37:02 记录到两次 MaiChat 进程建连；当时未记录异常堆栈。

## 定点证据

验证码入口依次是 `MainActivity` 的登录按钮、`RemoteIMSessionController.requestAccountCode`、
`MaiChatAuthClient.requestCode`、`MaiChatAuthClient.requestWithStatus`。后者用 Android
`HttpsURLConnection` 请求 `POST /api/v1/auth/sms/challenge`。近期提交没有改变这一段
HTTP 实现。`RemoteIMSessionController` 原样显示 `Exception.getMessage()`，因此原来的
logcat 没有失败阶段信息。

为了避免发送真实短信，在真机安装临时测试 APK，用格式无效的测试号码，以及格式
合法但 `age_confirmed_14_plus:false` 的测试请求走同一接口。两者都在服务端发送
短信之前被拒绝，理论上应分别收到 HTTP 400 和 403。两者均复现了 `Connection reset`。
后者在 10:44:31 的堆栈落于 `HttpsURLConnection.getOutputStream()` →
`ConscryptEngineSocket.doHandshake()` → `SocketInputStream.read()`，耗时 79ms，
请求体尚未发送。由此可确认这次失败不在短信供应商、HTTP 参数校验或 JSON 解析。

10:49 的同一手机对照探针显示：

| 目标 | 结果 |
|---|---|
| 活动网络 | 蜂窝网络；Wi-Fi 非活动网络 |
| `ichat.life`、`www.ichat.life` 系统 DNS | 均只得到 IPv4 `8.134.253.45`，没有 AAAA |
| 两个域名的 HTTPS 页面 GET | 均在 88–134ms 被 reset |
| `http://ichat.life/legal/terms.html` | HTTP 403，107ms；仅用于定位明文端口，不用于登录 |
| 直连该 IPv4，显式设置域名 SNI，TLS 1.2 与 1.3 | 四组均在握手时被 reset，耗时 39–172ms |
| 直连该 IPv4、不发送 SNI、TLS 1.2 | 握手成功；系统默认证书域名校验 `ichat.life` 通过；无凭据 GET 账号接口返回 HTTP 401 |
| 同手机访问 `https://www.baidu.com/` | HTTP 200，耗时 273ms |

Mac 直连正式账号接口，用不合法参数 POST，得到预期 HTTP 400 JSON；手机对正式
地址的 DNS、ICMP 和 TCP 443 建连也都成功。这说明服务不是整体离线，且失败
发生在手机蜂窝网络到该域名/IP 的 TLS 握手路径。无 SNI 成功、有两个域名的
SNI 均失败，使范围进一步缩到 SNI 相关的链路或入口策略。该结论尚不能仅凭
客户端日志区分运营商中间设备、云安全策略与服务器入口配置中哪一方发出了
TCP RST。未在正式客户端关闭 SNI 或放松证书校验。

服务器于 10:54:26–10:54:29 对同一蜂窝出口做了 `tcpdump` 定点对照。带 SNI 的
连接完成 TCP 三次握手后，服务器收到 517 字节 ClientHello，紧接着收到三份
来源地址为手机蜂窝出口的 TCP RST；服务器此前没有发出 ServerHello 或 RST，
35ms 后还尝试确认收到的 ClientHello。相隔约 2 秒，无 SNI 的连接完成 TLS
握手、证书传输及无凭据账号 GET，得到预期 401。服务器 Nginx 错误日志和
账号服务日志在原始故障时间没有对应业务错误。这进一步排除了 Nginx/Node
主动返回业务错误；RST 更像链路设备伪造客户端地址注入，但单侧抓包尚不能
证明具体设备归属。

## 处理与验证边界

当前未修改正式 App 或登录地址，也未对有效手机号发码或自动重试。`www` 域名在
同一真机上也失败，不能作为备用地址。下一步应对齐云服务器的 Nginx、系统防火墙
及云安全日志，并用同手机 Wi-Fi 与蜂窝网络各做无短信探针；还应核对域名备案及
接入状态，不能未经核实就把无 SNI 诊断路径作为正式 App 的绕行实现。定位具体
阻断策略后，再做最小修复并在真机走相同用例回归。
