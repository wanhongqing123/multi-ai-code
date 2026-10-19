# 多端 Session 同步服务上线记录（2026-10-20）

## 目标与连接排障

- 目标：把 `0f8d30eb658288e719074e1e05f2cbff1d81c11d` 的独立同步服务部署到
  现有阿里云 ECS，并验证账号鉴权及同账号跨设备路由。不在服务端执行模型或工具。
- Mac 到旧 ECS 的 SSH 在 banner 前断开；Windows 只读探测能收到 SSH banner。
  Mac 路由查询发现目标流量经 `utun5`，使用 `ssh -B en0` 指定 Wi-Fi 网卡后，
  现有授权密钥可直接登录。未复制私钥到 Windows，也未修改系统路由。
- 上线前账号服务与 Nginx 均运行；`/opt/maichat/auth/service.js` 与仓库只差
  `/v1/auth/me` 返回 `login_session_id`。同步服务在服务器上尚不存在。

## 上线内容

- 安装 `session-sync-server.js`、`session-sync-service.js`、`session-sync-store.js` 到
  `/opt/maichat/auth/`，由独立 `maichat-session-sync.service` 运行，监听
  `127.0.0.1:9102`，数据库只允许 `maichatsync` 用户读写。
- 账号服务只增加已认证 `/v1/auth/me` 的 `login_session_id` 字段，重启后健康检查
  返回 200；原无凭据 `/v1/auth/me` 仍为 401。
- Nginx 增加 `/api/v1/session-sync/` 独立路由及每 IP 600 次/分钟的限制；
  `nginx -t` 通过后热重载。未更改原账号路由、模型直连方式或 IM 配置。
- 备份为 `/opt/maichat/auth/service.js.before-session-sync-0f8d30eb` 和
  `/etc/nginx/conf.d/ichat.conf.before-session-sync-0f8d30eb`。

## 线上回读

| 文件 | SHA-256 |
|---|---|
| `service.js` | `316f508ccc3a5d253e29a8d4c72ae54be045f1e6eb60466ab6958e0265149fbe` |
| `session-sync-server.js` | `aadec04932cc5760c2fafb7e8a7323dafc2f3582ac49c7c9a7505207c20e43dc` |
| `session-sync-service.js` | `c59a6ca926354e339abec1d7553ea001e8bbf55619f2443d66f9dc842dbd8d56` |
| `session-sync-store.js` | `01084936ab9ca8a87a56a16e61f25a1d2a2b30a001144f7558b69ca2af1146f6` |
| Nginx `ichat.conf` | `0d022a28e300b4915dfe14753bf1cf62aa1ccf8fd9d650932bfee338e1dc8e12` |

- `maichat-auth.service`、`maichat-session-sync.service`、`nginx` 均为 `active`，
  同步服务 `/health` 为 200。Nginx 的 sessions、poll、presence、send、ack
  五个无凭据接口逐一返回 401。
- 使用原有、测试前没有活跃会话的测试账号，经**生产 Nginx HTTPS 路由**登录
  `mac` 与 `ios_phone` 两个名额；`auth/me` 均为 200 并返回同一内部账号 ID、
  各自登录代次。Mac 上报测试 Session 后手机目录能读到；手机发送只读
  `history_request`，相同消息 ID 再发得到同一收据且 `duplicate=true`；Mac
  只取到一份请求并确认。随后清空测试 Session 目录、两个名额均退出，回读该
  测试账号活跃会话数为 0。没有模型或付费工具请求，未输出访问令牌。
- 热重载后一瞬曾由旧 Nginx worker 返回 404；再次经相同路径检查稳定为 401。
  同步服务首次 `systemctl is-active` 已为 active 时，进程仍未完成 bind，首个
  `/health` 连接失败；稍后重试为 200，日志无崩溃。

## 尚未验证

- 客户端之间的真实 Session 与工具操作尚未跨网络走通。iOS 新包已保留数据安装，
  自动启动被设备锁屏拒绝，需解锁后继续；当前 Mac 正在运行的旧版 App 未替换。
- Mac 与 Windows 直连 `ichat.life:443` 的 SNI TLS 握手在网络路径上仍出现 reset；
  服务器本机 Nginx HTTPS 路由和无 SNI 的 IP 对照可用，不能据此推断所有客户端
  网络均可用。后续需以实际客户端登录与同步日志判定。
