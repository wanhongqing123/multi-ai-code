# MaiChat 统一账号服务

本服务与现有 `oss-media-signer` 分进程部署在同一台 ECS。Nginx 负责公网 TLS；
Node 服务只监听 `127.0.0.1:9100`。验证码、密码哈希、登录会话与腾讯 IM UserSig
均在服务端处理。客户端只获得自己的短期会话凭据和 UserSig，不获得 IM 签名密钥。

## 当前接口

| 方法与路径 | 功能 |
|---|---|
| `POST /v1/auth/sms/challenge` | `{phone,purpose?,age_confirmed_14_plus:true}`；发送验证码，返回挑战 ID；`purpose` 为 `login` 或 `reset` |
| `POST /v1/auth/sms/verify` | `{phone,challenge_id,code,age_confirmed_14_plus:true}`；首次验证手机号自动创建账号，返回会话 |
| `POST /v1/auth/password/login` | `{phone,password,age_confirmed_14_plus:true}`；已设置密码的账号登录 |
| `POST /v1/auth/password/set` | Bearer 会话 + `{new_password}`；近期登录后设置/修改密码 |
| `POST /v1/auth/password/reset` | `{phone,challenge_id,code,new_password,age_confirmed_14_plus:true}`；短信验证后重置密码并撤销旧会话 |
| `POST /v1/auth/refresh` | `{refresh_token}`；轮换访问令牌与刷新令牌 |
| `POST /v1/auth/logout` | Bearer 会话；撤销当前会话 |
| `GET /v1/auth/me` | Bearer 会话；查询当前账号与脱敏手机号 |
| `GET /v1/im/credential` | Bearer 会话；只为当前账号签发 Tencent UserSig |

接口经 Nginx 对外使用 `/api/v1/...`；服务内部路由仍为 `/v1/...`。
用户主键和腾讯 IM UserID 与手机号分离，换号不改变 IM 身份。首次登录前如需
沿用旧 IM UserID，必须由管理员**独立核实归属**后在服务器上执行
`MAICHAT_AUTH_DB_PATH=... node bind-legacy.js`，从 stdin 输入
`{"phone":"...","im_user_id":"..."}`。已有客户端可自行生成任意账号的 UserSig，
因此旧客户端登录态不能单独作为旧账号归属证据。一个手机号只能绑定一个旧 IM ID。

## 防护边界

- 默认只允许 `MAICHAT_AUTH_ALLOWED_PHONES` 白名单号码收码。正式公开注册前要先完成
  设备/人机风险校验、短信费用预算和后台配额；不能简单设置开放开关。
- 当前没有监护人身份及同意核验，因此发码、验证码登录、密码登录和重置密码
  都要求明确传入布尔值 `age_confirmed_14_plus: true`。这是用户自我声明，
  不能当作真实年龄核验或监护人同意。
- 验证码六位、五分钟有效、最多五次输入、只能用一次；同号一分钟内不重发，
  同号每天十次、同出口每天五十次。新验证码受理后旧码立即失效。
- 验证码校验记录和失败登录记录在安全追溯窗口后按天清理；失效或撤销的
  会话记录同样按天清理。服务启动时会先补做一次清理。
- 密码是可选登录方式，首次账号创建必须先验证手机号。密码至少 15 个字符，
  服务端用独立盐与 scrypt 慢哈希保存，App 不保存明文密码。
- 访问令牌 15 分钟、轮换刷新令牌 30 天；只存令牌哈希。退出、密码重置和
  重放旧刷新令牌会撤销对应会话。客户端应在 Keychain/Keystore/系统凭据库
  保存刷新令牌，访问令牌只在内存使用。
- 当前账号服务不代理大模型调用。现有共享服务令牌和 `/credentials` 原始 Key 下发
  是联调链路，**不能**因手机号登录服务存在就向公众开放。正式多用户发布前，
  需要用用户会话鉴权替代共享令牌，并让模型请求经服务端代理和按用户限额。
- 桌面端和已发布的旧版移动客户端仍可能持有旧 IM 签名密钥。完成迁移后需轮换
  该密钥；仅从新包移除明文无法撤销旧包和历史源码中的密钥。

## 安装与部署

```sh
npm ci --omit=dev
npm test
```

服务端配置参见 `deploy/auth.env.example`，只把实际值写入服务器上的
`/etc/maichat/auth.env`（权限 `0600`）。`MAICHAT_SMS_ACCESS_KEY_ID` / Secret
需要具备短信发送权限；签名和验证码模板必须已经审核可用。数据库放
`/var/lib/maichat-auth/`，目录仅授权服务账号，定期备份 SQLite 数据库与 WAL。
部署时使用示例 systemd 服务及 Nginx 路由，并保留现有素材签发服务。

测试使用模拟短信，不会联系真实号码：`npm test`。iOS 和 Android 已改为
手机号验证码／密码登录，并通过本服务领取 UserSig；两端代码构建通过，
Android 真机的完整短信和 IM 联调仍待验证。桌面端尚未迁移；
在三端都改为服务端凭据且完成旧 IM 身份映射之前，
本服务仍只作为测试环境能力。部署环境目前只允许指定测试手机号收码；
其他号码在正式开放注册后首次验证时会创建新 IM UserID。
