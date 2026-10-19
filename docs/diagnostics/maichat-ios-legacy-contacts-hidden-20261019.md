# iOS 旧联系人缓存被新好友列表过滤（2026-10-19）

## 现场与复现

- 用户 iPhone 14 Pro Max 安装 MaiChat 2.2.2 (90) 后，在“通讯录”看到旧分组“家庭电脑”和“mac 电脑以及 ssh 远程”，两个分组人数都是 0。
- 当前产品源码含 `e4d9c238` 的昵称与好友展示代码；账号好友主逻辑来自 `655785677`、`a848a72b1`。
- 只读导出该 App 数据容器的 `Library/Preferences/com.kongshang.maichat.plist`：`maichat_settings` 仍有 38 个 `friendUserIDs`、38 个联系人及原分组。没有卸载 App 或修改这份数据。
- 生产 MaiChat 账号库中，此手机号账号的 `friendships` 为 0；旧 `friend_mirror_sync` 记录的腾讯 IM 好友数和映射数也都是 0。腾讯账号只读 `account_check` 显示：38 个缓存 ID 中 37 个 IM 账号已存在，1 个未注册；38 个都尚无对应 MaiChat 账号。

## 根因与边界

`ContactsView.visibleContacts` 只显示 `acceptedFriendIMIDs` 中的联系人，而 `RemoteIMAppState.refreshAcceptedFriends()` 从 MaiChat 账号服务的已接受好友列表刷新该集合。升级前保存在本机的联系人仍存在，但不是新账号库的已接受好友，因此被过滤，分组仍显示而人数为 0。缓存联系人与腾讯 IM 已接受好友不是同一事实；不能仅凭本机缓存给现有 IM ID 建立可登录账号。

服务端管理员创建测试账号会先检查腾讯 IM ID 是否未注册；现有 37 个 ID 会被拒绝，以防占用已有身份。用户希望把旧联系人作为测试账号并统一使用 `123456`，这还与当前测试账号密码最少 15 字符的校验冲突。现有所有有效账号可凭登录会话领取 IM 凭据与模型 API Key，所以这批账号的归属及弱密码带来的权限范围需先确定。

## 恢复结果与未完成边界

- 已将 38 个 ID、原分组与腾讯 IM 注册状态整理成私有核对表发给用户。用户确认这批旧 IM 身份归其控制，要求 MaiChat ID 与 IM ID 相同，昵称为 `测试-{MaiChat ID}`。
- 生产数据库在迁移前备份到 `/var/lib/maichat-auth/backups/cached-contacts-20261019T060700Z/`，备份完整性 `ok`。服务端迁移代码为 `eee2cdef`，分批执行修订为 `860ac0dc`；线上副本 69 项测试通过，实际名单的无写入预检为 38 人、37 个已存在 IM ID、1 个需重新导入。
- `mac-approval-e2e` 的腾讯 `account_import` 返回 `70399`。[腾讯文档](https://cloud.tencent.com/document/api/269/1608?lang=zh)定义为删除后三个月内不能重新导入；脚本在写数据库前停止。核查仍为 18 个用户、0 条好友、无迁移标记，完整性 `ok`。用户明确让该账号暂不处理。
- 将其余 37 人再次无写入预检后一次性导入：生产账号库现有 55 个用户，目标账号具有 37 条好友关系；37 个测试账号的 MaiChat ID、腾讯 IM ID、昵称及用户指定密码的哈希均逐项核验通过。外键检查无错误，完整性 `ok`。未替换 iPhone App，本地 38 个旧联系人缓存与分组保留。
- 腾讯昵称同步队列已清空。腾讯 IM 双向好友同步成功 20 人，其余 17 人的返回码为 `30010`，[官方定义](https://cloud.tencent.com/document/product/269/1643)是发起方好友数达到系统上限；腾讯 `friend_get` 只读回查为 20 人。这 17 人仍在 MaiChat 的 37 条好友关系中，App 可从账号服务显示，但不能宣称腾讯好友关系已全部同步。服务端随后给容量错误设置延后重试，避免每 5 秒持续发起必然失败的请求。
- iPhone 的实际刷新画面和未完成的 17 人聊天路径尚未亲自验收；好友资料页为后续产品改动。
