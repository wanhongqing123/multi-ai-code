# MaiChat 统一账号服务

本服务与模型凭据服务、素材服务分进程部署在同一台 ECS。Nginx 负责公网 TLS；
Node 服务只监听 `127.0.0.1:9100`。验证码、密码哈希、登录会话与腾讯 IM UserSig
均在服务端处理。客户端只获得自己的短期会话凭据和 UserSig，不获得 IM 签名密钥。

## 多端 Session 同步服务（开发中）

`session-sync-server.js` 是独立进程，默认只监听 `127.0.0.1:9102`，每次连接
通过本机 `/v1/auth/me` 核验现有 MaiChat Bearer 会话。账号服务的 `auth/me`
增加稳定的 `login_session_id`；访问令牌刷新时该 ID 不变，同名额重新登录时改变。
同步服务持久化同账号设备和 Session 目录，临时保存待确认设备消息和不含正文的
去重收据。完整可执行历史仍在创建 Session 的设备，模型请求由该设备直连供应商。
四端使用同一套 C++ WebSocket++/独立 Asio 客户端，以 WSS 连接本服务。iOS
正常模拟器 App 已用本机假 Mac 验证目录、历史、文字与拒绝后的草稿保留；
Android 模拟器完成 JNI/WSS 握手及远端会话界面测试；Mac 和 Windows
完成桌面构建和路由测试。生产 WSS 的账号握手与空 Session 上报已验证，
真实所属设备的四端互控与跨设备媒体仍待验收。

正式协议入口为 `wss://ichat.life/api/v1/session-sync/ws`，握手需要 MaiChat Bearer。
客户端发送 `{id,op,body}`，服务用 `{type:"response",id,status,body}` 应答，
并主动推送 `catalog` 和 `inbox`：

| `op` 或推送 | 用途 |
|---|---|
| `presence` | 当前设备报告安装实例、设备名与本地根 Session 快照；约每 15 秒更新 |
| `catalog` 推送 | 同账号 Session 目录变化时立即推送，45 秒无上报标记离线 |
| `inbox` 推送 | 向当前登录设备投递消息；客户端处理后明确确认 |
| `send` | 给同账号指定设备的指定 Session 送唯一 ID 的操作或可见事件 |
| `ack` | 确认收件，服务随后清理临时正文；收据保留 30 天防重复提交 |

已发布的旧客户端仍可使用原 HTTP `/api/v1/session-sync/` 路由，升级期间保留；
新客户端不轮询它。每条设备消息的 JSON 载荷最多 64 KiB，请求帧最多 96 KiB；长历史由所属设备分页发送，
大文件另按需分块。`send` 的 200 只表示临时信箱已接收，客户端仍须等待所属设备的
执行确认。带副作用的 `command` 首次取件后不自动重投；可见事件在确认前可重投。
服务重启后所有设备先显示离线，直到重新上报心跳。

见 [完整产品流程与验收边界](../../docs/design/maichat-multidevice-session-sync-20261019.md)。
部署时在同一 ECS 上为 `maichatsync` 创建独立的可写目录
`/var/lib/maichat-session-sync`，安装同目录提供的 systemd、Nginx 与环境文件示例；
先更新账号服务，再启动同步服务。`/health` 仅表示进程可响应，不代表有在线设备。

## 当前接口

| 方法与路径 | 功能 |
|---|---|
| `POST /v1/auth/sms/challenge` | `{phone,purpose?,age_confirmed_14_plus:true}`；发送验证码，返回挑战 ID；`purpose` 为 `login`、`reset` 或 `social_bind` |
| `POST /v1/auth/sms/verify` | `{phone,challenge_id,code,age_confirmed_14_plus:true,device_slot,device_platform?}`；首次验证手机号自动创建账号，返回会话 |
| `POST /v1/auth/password/login` | `{account_id,password,age_confirmed_14_plus:true,device_slot,device_platform?}`；用 MaiChat ID 在手机、平板或桌面登录。旧手机号密码请求仍接受 `{phone,password,...}` |
| `POST /v1/auth/password/set` | Bearer 会话 + `{new_password}`；近期登录后设置/修改密码 |
| `POST /v1/auth/password/reset` | `{phone,challenge_id,code,new_password,age_confirmed_14_plus:true,device_slot,device_platform?}`；短信验证后重置密码并撤销旧会话 |
| `POST /v1/auth/social/wechat/login` | `{code,device_slot,device_platform?,age_confirmed_14_plus:true}`；服务端向微信兑换 code；已绑定返回会话，未绑定返回 202 手机绑定票据 |
| `POST /v1/auth/social/qq/login` | `{access_token,device_slot,device_platform?,age_confirmed_14_plus:true}`；服务端向 QQ 验证 SDK token 和 AppID；已绑定返回会话，未绑定返回 202 手机绑定票据 |
| `POST /v1/auth/social/bind` | `{binding_token,phone,challenge_id,code,age_confirmed_14_plus:true}`；消费 `social_bind` 短信码，将已验证的社交身份绑定到该手机号账号并返回会话 |
| `POST /v1/auth/refresh` | `{refresh_token}`；轮换访问令牌与刷新令牌 |
| `POST /v1/auth/logout` | Bearer 会话；撤销当前会话 |
| `GET /v1/auth/me` | Bearer 会话；查询当前账号与脱敏手机号 |
| `POST /v1/auth/profile` | 有效 Bearer 会话，传 `{nickname}`；设置自己的 MaiChat 昵称，空字符串清空。最多 40 个 Unicode 字符且不超过 120 字节；结果排队同步到腾讯 IM。 |
| `POST /v1/auth/desktop/request` | 桌面端提交 `{platform:"mac"或"windows",code_challenge}`，返回 `request_id`、`qr_uri`、`qr_png_base64`、`display_code`、`expires_in:120` |
| `GET /v1/auth/desktop/request/{request_id}` | 有效移动端 Bearer；首次查询把 `pending` 标记为 `scanned`，返回 `{platform,display_code,status,expires_in}` |
| `POST /v1/auth/desktop/approve` | 有效移动端 Bearer + `{request_id}`；确认展示码后批准桌面登录 |
| `POST /v1/auth/desktop/deny` | 有效移动端 Bearer + `{request_id}`；拒绝桌面登录 |
| `POST /v1/auth/desktop/cancel` | 桌面端提交 `{request_id,verifier}`；更换二维码或关闭登录时取消请求，也可撤销响应丢失时已领取的会话 |
| `POST /v1/auth/desktop/claim` | 桌面端提交 `{request_id,verifier}`；等待批准返回 202，批准后一次性返回会话 |
| `GET /v1/im/credential` | Bearer 会话；只为当前账号签发 Tencent UserSig |
| `GET /v1/model/seedance-credential` | 有效 Bearer 会话；返回 5 分钟、限定 Seedance 与 Seedream 的临时方舟 Key；路径保留旧名称兼容 App |
| `GET /v1/model/bailian-credential` | 有效 Bearer 会话；返回 5 分钟百炼临时 Key 和万相 Workspace ID，供万相与百炼可灵共用 |
| `GET /v1/model/api-keys` | 有效 Bearer 会话；返回 GLM、DeepSeek、MiniMax Key，客户端直接调用供应商 |
| `GET /v1/media/authorize` | 有效 Bearer 会话；供素材服务核验该账号可否登记素材或取得 OSS 上传签名，不返回密钥 |
| `POST /v1/asr/realtime-url` | 有效 Bearer 会话且空 JSON 请求体；返回一次性 `voice_id`、5 分钟有效的腾讯实时语音识别 `url` 与 Unix 秒 `expires_at`。每账号每分钟最多签发 10 次，未配置返回 503，超过频率返回 429；音频由 App 直连腾讯，不经过账号服务。 |
| `GET /v1/friend/my-qr` | 有效 Bearer 会话；将 MaiChat `account_id` 编入好友二维码，不含手机号或凭据。 |
| `POST /v1/friends/resolve` | 有效 Bearer 会话，传 `{account}`（MaiChat ID 或精确手机号）；返回 MaiChat `account_id`、昵称与当前 `im_user_id`；每账号每小时最多 20 次。 |
| `POST /v1/friends/requests` | 有效 Bearer 会话，传 `{account}`；创建待确认的 MaiChat 好友申请，同一发起人每小时最多 20 次。 |
| `GET /v1/friends/requests` | 有效 Bearer 会话；返回待处理的 `incoming` 与 `outgoing`。 |
| `POST /v1/friends/requests/{id}/accept`、`/reject` | 接收方同意或拒绝申请；同意后 MaiChat 关系立即生效，腾讯 IM 同步排队。 |
| `GET /v1/friends` | 有效 Bearer 会话；返回 MaiChat 已接受好友的账号、昵称及当前 IM 映射，不从腾讯列表构建。 |
| `POST /v1/friends/remove` | 有效 Bearer 会话，传 `{account}`；删除 MaiChat 好友关系，异步删除腾讯双向关系。聊天历史不删除。 |
| `POST /v1/friend/resolve` | 旧客户端过渡接口，仅按手机号返回 IM ID；新客户端不得使用。 |

接口经 Nginx 对外使用 `/api/v1/...`；服务内部路由仍为 `/v1/...`。

## 注销与举报运营处理

已登录用户通过 `GET /v1/auth/account/deletion/options` 查看确认方式，使用
`POST /v1/auth/account/deletion/challenge` 获取手机号验证码（有手机号时），再以
`POST /v1/auth/account/deletion` 提交账号 ID 和密码或验证码。提交成功立即停用账号、
撤销账号服务会话，返回申请编号和收据令牌；无需登录的
`POST /v1/auth/account/deletion/status` 使用两者查询处理状态。处理期限为 30 天。
已登录用户使用 `POST /v1/safety/reports` 举报账号或消息，并可用
`GET /v1/safety/reports` 查看自己提交的举报。仅经 SSH 转发访问的管理页列出待处理
注销与举报，管理员在核查后填写举报处理结果。**管理页没有自动通知**，运营人员需
每天检查待处理项及到期时间。

注销申请的后台清理是人工核查流程，不因 HTTP 返回 `202` 而自动完成：

1. 在服务端加载账号服务环境，执行 `node complete-account-deletion.js --list`，
   根据申请编号核对当前账号。先备份账号库与设备同步库。
2. 分别对同一申请编号运行 `node purge-tencent-account.js <申请编号>` 和
   `node purge-session-sync-account.js <申请编号>` 预检；核对后用各自的
   `--execute <申请编号>` 执行。后者还需要设备同步库路径环境变量。
3. 在素材服务环境中对该 MaiChat ID 运行 `node purge-account-assets.js <账号>`
   预检，再用 `--execute <账号> <同一账号>` 清理本服务能归属的 OSS 对象。旧版未带
   账号前缀的对象无法靠此脚本识别，必须结合申请人的素材记录人工核查；Ark
   私有素材及已发送给其他用户的内容也须逐项核查。
4. 将腾讯 IM、设备同步、OSS 与 Ark、共享内容四项结果和操作人记录到权限为
   `0600` 的证据 JSON，确认各项完成后执行
   `node complete-account-deletion.js --complete <申请编号> <证据文件>`。完成前不得
   填写未核实的证据。最后通过收据查询接口回读 `completed`。

以上脚本均为服务端操作入口；普通 App 不直接调用清理命令。未清理完毕的申请保留
`pending`，不得仅为满足期限而标记完成。相关测试账号也遵循同一流程。

## 好友关系主记录与旧镜像导入

`friend_requests` 和 `friendships` 是 MaiChat 的申请及好友主记录，好友以用户内部主键关联；
对外使用短 `account_id`。接受或删除时写入 `friend_im_sync`，服务端每 5 秒把期望状态
通过管理员 REST API 同步到腾讯 IM；失败保留队列并重试。腾讯 IM 是当前通信服务的
关系投影，客户端通讯录以 MaiChat 列表为准。旧用户的内部 UUID 保留为数据库关联键，
迁移时为其补发短 `account_id`，现有会话与手机号绑定不改；新用户内部键和短账号相同。

上线前先运行旧只读镜像 `friend-mirror.js --run`，再用
`node import-friend-mirror.js --dry-run` 查看互为好友的关系数；备份数据库后运行
`node import-friend-mirror.js --run` 一次性导入互为好友的已注册账号。单向关系不会
自动升级为双向好友。导入后停用旧的 `maichat-friend-mirror.timer`，后续不再让腾讯列表
覆盖 MaiChat 主记录。导入有不可重复标记，删除好友后不会从旧镜像重新出现。

## 昵称主记录

`users.nickname` 是可空的 MaiChat 账号资料；未设置时 App 展示 MaiChat ID。登录、刷新、
`auth/me` 与好友接口读同一列。用户修改或清空昵称时先原子保存 MaiChat 账号库，再写入
`profile_im_sync`；服务端每 5 秒把相同值投影到腾讯 IM 的 `Tag_Profile_IM_Nick`。
失败保留队列重试，写入期间用户再次修改昵称时旧同步结果不能删除新任务。
腾讯 IM 昵称不回写 MaiChat 账号库，也不用于账号身份或好友关系主键。

### 原有镜像的范围

账号服务的 `friend_mirror` 用 MaiChat 内部 `users.id` 记录已注册账号间的有向好友关系，
`friend_mirror_sync` 记录每个账号最近一次成功同步时间和腾讯 IM 好友数量。手机号和
腾讯 IM ID 不进入关系主键。镜像只作为旧数据导入来源，不参与新好友申请、消息发送或权限判断。

`node friend-mirror.js --dry-run` 用现有 IM SDKAppID/密钥生成管理员 UserSig，调用腾讯
`friend_get` 分页读取全量好友，**不写数据库**；`--run` 在每个账号完整拉取成功后
原子替换该账号镜像。单个账号拉取失败会保留该账号上次成功的快照，进程报错供
systemd 监控。只映射账号库中已存在的好友，未注册的外部 IM ID 不写入镜像。
旧定时任务示例见 `deploy/maichat-friend-mirror.service.example` 与 `.timer.example`；
迁移前手动执行一次 `--run` 并核对摘要，迁移后停用 timer。任务只读腾讯 IM，不调用
添加、删除好友接口，不需要修改腾讯控制台的全局回调配置。默认管理员账号为
`administrator`、REST 域名为 `https://console.tim.qq.com`，如应用不在中国区可通过
`MAICHAT_IM_ADMIN_USER`、`MAICHAT_IM_REST_ORIGIN` 覆盖。

## 测试账号管理员界面

在服务器的 `/etc/maichat/auth.env` 设置独立的 `MAICHAT_AUTH_ADMIN_SECRET`（至少 32 字符）
后，服务另外只监听 `127.0.0.1:9101`。管理员通过 SSH 端口转发访问这个本机页面，
用 HTTP Basic 的用户名 `admin` 和该独立密钥登录。页面可填写测试账号的 MaiChat ID 与
密码、查看状态并停用；不显示已保存的密码。测试账号无手机号，内部 ID、对外
`account_id` 和当前腾讯 IM ID 使用管理员填写的同一个值。账号 ID 限 3～32 字符，
首字符小写字母，其余为小写字母、数字、下划线或连字符；密码沿用 15～128 字符的
scrypt 策略。创建前服务端通过腾讯 `account_check` 确认该 IM ID 尚未被其他人占用，
再用 `account_import` 建立 IM 账号；腾讯不可用时拒绝创建，不接管已有 IM ID。
页面不经 Nginx 对公网提供，创建和停用留 `admin_audit` 记录。

旧版 App 曾在本机缓存尚未接入 MaiChat 账号体系的腾讯 IM 联系人。经账号所有者核实
归属后，可用 `import-cached-test-contacts.js` 一次性将这些 ID 绑定为同名 MaiChat 测试账号，
并建立经核对的 MaiChat 好友关系。迁移文件需逐项列出 `im_user_id` 与预期腾讯状态；
先用 `--dry-run` 对照当前账号库和腾讯 `account_check`，备份数据库后才运行 `--run`。
脚本不会接管已存在的 MaiChat 账号，只对腾讯尚未注册的 ID 执行 `account_import`；
每个迁入账号的昵称为 `测试-{MaiChat ID}`，好友和昵称变更进入原有 IM 同步队列。
迁移密码从 `MAICHAT_IMPORT_PASSWORD` 读取，不作为命令行参数或日志输出。
仅此受控迁移允许 6～128 字符的密码；管理员页面创建新测试号仍要求至少 15 字符。

用户主键和腾讯 IM UserID 与手机号分离，换号不改变 IM 身份。首次登录前如需
沿用旧 IM UserID，必须由管理员**独立核实归属**后在服务器上执行
`MAICHAT_AUTH_DB_PATH=... node bind-legacy.js`，从 stdin 输入
`{"phone":"...","im_user_id":"..."}`。已有客户端可自行生成任意账号的 UserSig，
因此旧客户端登录态不能单独作为旧账号归属证据。一个手机号只能绑定一个旧 IM ID。

## 设备名额与桌面扫码

短信验证、密码登录和密码重置只能申请移动端名额：`ios_phone`、
`android_phone`、`tablet`。平板另需传 `device_platform:"ios"` 或 `"android"`，
两个系统的平板共用一个名额。手机可省略 `device_platform`，服务端会按名额填入
`ios` 或 `android`。同一账号的五个名额（以上三个，加 `mac`、`windows`）
各只保留一个有效会话；新登录会撤销同名额旧会话。登录与刷新响应带顶层
`device_slot`、`device_platform`；被替换的旧会话访问受保护接口或刷新时返回
HTTP 401，`code:"SESSION_REPLACED"`，客户端应退出 IM 并提示设备被替换。
账号主键和 IM UserID 在这些登录方式之间保持不变。

`MAICHAT_AUTH_REQUIRE_DEVICE_SLOT=0`（默认）时，旧版客户端没有传
`device_slot` 仍可取得未分类会话，响应中的两个设备字段为 `null`；未分类会话
不能查询或批准扫码请求。切换为 `1` 并重启服务时，已有未分类会话立即撤销；
其访问和刷新返回 401、`code:"DEVICE_SLOT_REQUIRED"`。新登录不带名额返回
400、同一错误码；缺少有效移动名额的请求不会消耗短信验证码。服务还会拒绝并
撤销切换后从旧进程写入的未分类会话。其他配置值会使服务拒绝启动，避免误以为
约束已开启。业务会话的撤销不会立即断开已建立的腾讯 IM 连接；已更新客户端
收到错误后应主动登出，旧客户端则可能保持 IM 连接直到腾讯侧会话失效。

设备类别由客户端声明，服务端按声明实施名额；HTTP `User-Agent` 不作为
设备证明。桌面程序若恶意自称移动端，仍能调用手机号登录接口。要对不可信
客户端强制“桌面仅扫码”，还需要可信设备证明或等效的设备绑定机制。

桌面端每次生成新的 32 字节随机 `verifier`，计算
`code_challenge = base64url(SHA256(verifier 原始字节))`。`verifier` 仅留在桌面端；
二维码 `qr_uri` 为 `maichat://auth/desktop?request_id=<UUID>`，不包含凭据。
`qr_png_base64` 是二维码 PNG 文件的原始 base64，可直接解码显示。手机扫描后
读取请求详情，向用户展示目标平台和六位 `display_code`；用户核对桌面上的号码
再批准。扫描只标记 `scanned`，不锁定审批账号；最终批准者的账号会归属桌面会话。
桌面端可每两秒调用 `claim`：`pending` 或 `scanned` 返回 202 和对应状态，
拒绝或审批会话已撤销返回
403，请求过期或已兑换返回 410；成功后获得桌面名额对应的会话。请求两分钟
有效、只能兑换一次，同来源 IP 十分钟最多创建十次。只有有效的移动端名额会话
可查询或审批，其他会话返回 403、`code:"MOBILE_SESSION_REQUIRED"`。桌面更换
二维码或关闭登录时，使用同一个 `verifier` 调用 `cancel`；`pending`、`scanned`、
`approved` 均可取消，成功和重复取消都返回 200、`status:"canceled"`。取消后
手机查询与审批、桌面兑换均返回 410。若 `claim` 已提交但成功响应丢失，取消
会撤销该请求确切签发的桌面会话，不影响后来占用同一名额的新会话。桌面端成功
收到并保存会话后不应再取消该请求。升级前已兑换的旧请求没有所签会话 ID，
无法安全撤销，取消会返回 409。

## 微信与 QQ 身份绑定

移动端微信登录提交一次性授权 `code`，服务端使用配置在本机的微信 AppID 和
AppSecret 向微信官方接口兑换；QQ 登录提交 SDK 返回的 `access_token`，服务端
向 QQ 官方 `oauth2.0/me?fmt=json` 查询，并验证返回的 `client_id` 与配置的
QQ AppID 一致。OpenID 只取官方响应，客户端提交的 `openid` 或 `unionid` 不参与
身份判断；数据库只保存带密钥的身份哈希。两个移动平台应使用同一微信移动应用
AppID 和同一 QQ 应用 AppID，避免同一平台账号产生不同应用范围的 OpenID。
社交身份索引依赖 `MAICHAT_AUTH_HMAC_KEY`；轮换此密钥前必须迁移现有身份映射，
否则已绑定的微信、QQ 身份将无法再找到原账号。此密钥需要与账号数据库一同备份。
此 QQ SDK token 核验流程不使用 QQ AppKey；若以后采用 QQ 服务端 code 兑换，
AppKey 必须只保存在服务端。

未绑定身份返回 HTTP 202：`{binding_token,expires_in:600,phone_binding_required:true}`，
此时不创建账号或登录会话。客户端用现有短信接口发码，指定
`purpose:"social_bind"`，再把手机号、挑战 ID、验证码和绑定票据交给
`/v1/auth/social/bind`。短信码和票据均只能使用一次。绑定到已有手机号时复用
其用户主键及腾讯 IM UserID；首次验证新手机号则按现有规则创建账号。一个社交
身份不能改绑到别的账号，同一账号每种社交平台最多绑定一个身份。社交登录
始终要求有效的手机或平板名额，即使旧客户端兼容开关仍为 `0` 也是如此。

未配置微信 AppID/AppSecret 或 QQ AppID 的接口返回 503、
`code:"SOCIAL_PROVIDER_UNAVAILABLE"`，不会接受客户端声称的身份；官方拒绝的
凭据返回 401、`code:"SOCIAL_CREDENTIAL_INVALID"`。账号服务当前只提供后端
接口和移动端的账号绑定流程；微信、QQ 官方 SDK 接入及真实平台联调尚未完成。

微信开放平台创建移动应用时，名称和英文名称可填 `MaiChat`，简介应与实际
聊天、图片及视频创作功能一致。iOS Bundle ID 与 Android 包名均为
`com.kongshang.maichat`。应用官网应是可公开访问、含产品介绍及下载入口的
页面；仓库现有 `services/legal/site/index.html` 主要提供用户协议和隐私政策
入口，若以此页作为官网，提交审核前仍需补齐产品介绍。移动应用图片须按
控制台当前要求由正式应用商店图标制作，
不能以微信图标或临时占位图代替。Android 发布签名 MD5 必须取正式签名证书，
不能填开发调试签名；iOS Universal Link 必须先完成域名关联配置。

## 防护边界

- 所有格式合法的中国大陆手机号都可以申请验证码；原短信号码白名单不再参与发码。
  现有同号、同来源限频仍生效。公开注册时应持续监控短信费用并配置后台配额。
- 当前没有监护人身份及同意核验，因此发码、验证码登录、密码登录和重置密码
  都要求明确传入布尔值 `age_confirmed_14_plus: true`。这是用户自我声明，
  不能当作真实年龄核验或监护人同意。
- 验证码六位、五分钟有效、最多五次输入、只能用一次；同号一分钟内不重发，
  同号每天十次、同出口每天五十次。新验证码成功发出后旧码失效；若短信网关
  对并发请求乱序完成，较早请求不会覆盖较新已送达的验证码。
- 验证码校验记录和失败登录记录在安全追溯窗口后按天清理；失效或撤销的
  会话记录同样按天清理。服务启动时会先补做一次清理。
- 普通用户通过验证手机号创建账号，密码是可选登录方式；管理员可创建无手机号
  的测试账号并填写密码。密码至少 15 个字符，
  服务端用独立盐与 scrypt 慢哈希保存，App 不保存明文密码。
- 访问令牌 15 分钟、轮换刷新令牌 30 天；只存令牌哈希。退出、密码重置和
  重放旧刷新令牌会撤销对应会话。客户端应在 Keychain/Keystore/系统凭据库
  保存刷新令牌，访问令牌只在内存使用。
- 账号服务不代理大模型或媒体字节。Seedance 与百炼临时 Key 都由账号服务经本机回环
  请求模型凭据服务签发，火山 AK/SK、百炼永久 API Key 不下发。百炼临时 Key 继承
  主 Key 的模型权限，5 分钟内如泄漏仍可使用；业务配额仍须在百炼控制台限制。
  GLM、DeepSeek、MiniMax 由账号服务按登录会话下发。它们目前是供应商长期 Key，
  会进入客户端进程；退出登录无法撤销已被复制的 Key，密钥保护方式留待后续方案。
  服务端只下发凭据，绝不转发模型请求或流式响应。
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
部署时使用示例 systemd 服务及 Nginx 路由，并独立启动模型凭据与素材服务。

设备名额上线顺序：

1. 先备份账号数据库，以 `MAICHAT_AUTH_REQUIRE_DEVICE_SLOT=0` 部署新服务及
   数据库迁移，确认旧版手机仍能登录、刷新，扫码只能由带移动名额的新会话批准。
2. 发布并验证会提交正确 `device_slot` 的 iOS、Android 手机和平板版本；
   让客户端在收到 `SESSION_REPLACED` 或 `DEVICE_SLOT_REQUIRED` 后退出 IM 并
   展示重新登录提示。腾讯 IM 控制台需允许多平台登录，Android 同平台实例数
   至少为 2，才能同时保留 Android 手机和平板。
3. 确认准备停止旧版登录后，排空旧账号服务进程，将配置改为 `1` 并启动新服务。
   启动会撤销所有未分类会话，旧客户端必须升级才能再次登录；这一步会让仍在线
   的旧版用户退出。
   配置退回 `0` 也不会自动恢复已撤销的会话，用户需要重新登录。
4. 用五类设备分别验证登录、同名额替换、扫码批准和旧版请求拒绝；观察登录
   错误与短信发送情况，再决定是否继续保持强制开关。

旧配置 `MAICHAT_AUTH_ALLOWED_PHONES` 和 `MAICHAT_AUTH_OPEN_REGISTRATION`
不再影响发码。旧数据库中的 `sms_allowed_phones` 和 `auth_settings` 表无需
清理，也不再被读取；保留它们不会限制任何合法号码。

测试使用模拟短信，不会联系真实号码：`npm test`。iOS、Android 和桌面端的
源码已接入统一账号服务；正式服务部署、Android 真机短信与 IM 联调、桌面扫码
与移动端在生产环境的联调、微信和 QQ 官方 SDK 接入仍待完成。在旧 IM 身份
映射和密钥轮换完成之前，本服务仍只作为测试环境能力。新号码首次完成验证码
验证时会创建新 IM UserID。
