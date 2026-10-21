# MaiChat 上架准备服务端部署与验收（2026-10-21）

## 现场与复现

- 平台：`ichat.life` 生产 ECS，Node 22.23.0；源码起点
  `5b1d99f3c4d8003147d5691b48e772dc548a3939`。用户发现 App Store 所需的
  注销／举报功能已在仓库中，但线上仍是旧接口和旧隐私政策。
- 部署前经生产 Nginx HTTPS 请求
  `/api/v1/auth/account/deletion/options`、`/api/v1/safety/reports`，无凭据均为
  **404**；`/legal/privacy.html` 仍为 10 月 11 日版本。数据库有 55 个账号，
  `quick_check=ok`，尚无新业务表。
- 线上 `service.js`、`store.js`、`admin-server.js` 的 SHA-256 分别为
  `316f508ccc3a5d253e29a8d4c72ae54be045f1e6eb60466ab6958e0265149fbe`、
  `f14c0d57ea78dcd8dbbc0f4b638846df9c591cf2e9bd67d28241528e284872e9`、
  `308ee3e1de62398461e6e024f5804238ab9fec585b793f0f7b1e6484db8dc9e2`。
  与仓库比较只缺本次账号注销、举报及相关表／管理页代码；原有设备登录代次
  `login_session_id` 已在新代码中保留。

## 部署

- 先将旧服务代码、隐私政策、用户协议和 SQLite 在线一致性备份放在服务器
  `/var/lib/maichat-auth/backups/appstore-readiness-20261021T081938Z/`，数据库副本
  `quick_check=ok`、账号数 55。将新 `store.js` 对该副本迁移试跑后，账号数仍为
  55，外键错误 0，新增 `account_deletion_requests` 与 `safety_reports` 两表。
- 更新 `/opt/maichat/auth/` 的 `service.js`、`store.js`、`admin-server.js` 和五个
  注销运营脚本；重启 `maichat-auth.service`。更新
  `/opt/maichat/media/index.js` 的账号归属对象路径和 OSS 清理脚本，重启
  `maichat-media.service`。发布 `/var/www/ichat/legal/` 的隐私政策、用户协议。
  Nginx 和设备同步服务配置未改动。
- 线上回读 SHA-256：账号服务 `service.js` 为
  `6558c46f87a1e94dd43447477d847d531e7bf14b2cf677984848fbc5`，`store.js`
  为 `adee351a22424b12632e65b816aa93fb6934073bb39fb34be8663800edd5be6b`，
  `admin-server.js` 为
  `388c40cba045f4cee0bdbcc8b94888a3f0eb5049ea7447ec50f96e0e4ee55bb9`；
  媒体服务 `index.js` 为
  `4718b4dddd4033ab074c15c5c6c9d85668c5c4da1c60591064330818fbe56664`；
  隐私政策与用户协议分别为
  `0af73cc238838b3d3bbda50c62fdbcaf7556504a078c90938afee75460c67d57`、
  `87cfbcf936b50047ed33b6d709778c502861d729fc9cae9cc26cc45c044f57e3`。

## 验收与排障

- 本地定向测试：账号服务 43/43、媒体服务 22/22，通过；新表迁移已在生产数据
  副本预演。真实 Nginx HTTPS 上，无凭据查询注销／举报为 401，非法注销收据为
  400，`auth/me` 仍为 401；隐私政策和用户协议均为 200，新文案可回读。
- 使用两只新建的隔离测试账号，经生产 Nginx HTTPS 完成密码登录、`auth/me`、
  注销选项、错误账号确认拒绝、举报提交与本人列表、后台处理与状态回读。
  媒体服务签发的头像上传 URL 包含当前账号前缀；未上传任何文件。
- 两个账号均实际发起注销：服务返回 202，旧会话立即为 401，收据状态先为
  `pending`。腾讯 IM 账号删除结果均成功；设备同步库中对应行均为 0；OSS
  六类账号前缀均为空；测试没有 Ark 素材或聊天消息。按证据完成注销后，收据
  状态均为 `completed`；生产库回读测试账号 0、待处理注销 0、测试举报 0，
  `quick_check=ok`、外键错误 0。
- 首轮清理测试账号时，OSS 对象查询返回 `SignatureDoesNotMatch`。定点对照发现
  `purge-account-assets.js` 在第一页将 `marker: undefined` 也传给 `ali-oss`；
  同样请求省略 `marker` 后成功。修复提交
  `3ec947a6bfcd15943f751bc59f7ddef480b62658` 只在已有分页标记时传该参数，
  增加首轮调用断言；生产回读修复后的脚本 SHA-256 为
  `aa3aaffef5ff387ff14d70f40ac6aee9acc6aa932296accbf81e92f498d89dbf`。
  修复后重走原 OSS 查询与两账号注销收尾，全部通过。
- 最终 `maichat-auth`、`maichat-media`、`maichat-session-sync`、Nginx 均为
  `active`；同步服务本机 `/health` 为 200，媒体签名无凭据为 401。

## 边界与后续运营

- 没有从用户手机上的 iOS UI 再走一次生产注销界面；此次完成的是生产 API 与
  真实临时账号的端到端验收。没有触发模型请求或付费生成任务。
- 老版没有账号前缀的 OSS 对象和 Ark 私有素材不能靠前缀脚本自动归属，真实用户
  注销需要运营人员逐项核查。待处理注销／举报当前由 SSH 转发的管理页供人工
  查看，没有自动提醒；需每天检查并在 30 天期限内处理。操作步骤见
  `services/auth/README.md`。
- 腾讯 IM 云端内容审核没有开通。App Store Connect 构建处理、元数据填写与
  苹果审核状态不属于本次服务器部署验收。
