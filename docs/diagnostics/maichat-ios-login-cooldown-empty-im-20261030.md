# iOS 登录短信倒计时与 IM 空白会话

## 需求和操作

- 普通手机号登录发送验证码成功后，至少 60 秒才能重新发送；倒计时不限制提交验证码登录。
- 修改号码、切换密码登录、返回验证码登录和 App 后台恢复后，原号码的截止时间仍有效。
- 空的 IM 会话保留白色消息区域，不展示图标、标题和操作提示；群聊和私聊的消息不能串到另一个会话。

## 源码与实现

2026-10-30，基于 `b275438b9fc40b299ded6a59c2218ba65a22dfce` 的工作区改动，
iOS 版本 `2.2.4 (141)`。

`AccountCodeCooldown` 以规范化号码记录绝对截止时间，合并同号码进行中的发送请求。
发送成功采用 `max(60, server.resendAfter)`，失败释放发送门禁；
`RemoteIMAppState` 持有状态，避免 SwiftUI 页面重建重置倒计时。
页面每秒刷新显示，回到前台重新计算剩余时间；换号清理旧验证码挑战，旧请求迟到不能覆盖新号码。
`ChatView` 删除空态覆盖层，沿用时间线已有的白色背景。

## 实际验证

- 生产倒计时状态的三个测试通过：截止时间、发送失败、号码隔离。
- 正常 Debug App 在 iPhone 17 Pro / iOS 26.5 模拟器运行；
  `SocialLoginUITests/testNormalPhoneLoginRetainsSmsCooldownAfterChangingPhone`：1 通过、0 跳过。
  结果 `/private/tmp/maichat-ios141-sms-ui.xcresult`。
  已查看截图：58 秒时重新发送不可用，验证码提交仍可用；切回原号码显示仍在进行的 53 秒倒计时。
- 复用已有群聊/私聊隔离 UI 用例：1 通过、0 跳过。
  结果 `/private/tmp/maichat-ios141-im-empty-ui.xcresult`；录屏
  `/private/tmp/maichat-ios141-im-empty-flow.mp4`。已查看空私聊截图，消息区为白色，群聊内容未泄漏。
- 最新 ASR V2 与登录预取改动后的正常 App 增量编译通过，日志
  `/private/tmp/maichat-ios141-asr-v2-final-build.log`。

短信验证使用本机假账号服务，未发送真实短信、未提交生产账号登录。
这些结果证明模拟器上的交互和状态；此记录不代表 `141` 已安装到物理手机，
实际安装与发布状态以最终交付记录为准。
