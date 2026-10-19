# iOS 本机 Session 同步验收排障（2026-10-20）

## 复现与证据

- 平台：Mac 上的 iPhone 17 Pro / iOS 26.4 模拟器，正常 MaiChat Debug App，
  本机 `services/auth/session-sync-ui-fixture.js` 提供测试账号与假 Mac Session。
- 以 `CODE_SIGNING_ALLOWED=NO` 构建的 App 首次自动登录时停在手机号页。
  App 诊断日志显示 `local-account-login-failed`、Keychain 错误 `-34018`。
- 登录测试账号后，首次同步心跳仍失败。模拟器日志每两秒出现
  `[AIHistory] session-sync failed: 网络连接失败`，本机服务只看到 `mac` 设备。
  定点代码显示 `MaiChatHttpTransport.cpp` 拒绝所有带 Bearer 的 HTTP 请求；
  账号请求所用的 loopback 传输允许本机 HTTP，但原先拒绝轮询查询串。
- 后续 UI 自动化能发现远端行却点不到时，XCTest 返回侧栏项负坐标。
  录屏证明抽屉未打开；改用产品已有的左缘右滑手势后可点选。

## 修正与验证

- 只在 Debug 模拟器且显式使用本机 UI 测试服务时，把刷新令牌留在进程内，
  避免无签名测试包访问 Keychain；正式设备继续使用 Keychain。
- 本机 Session 同步请求复用账号客户端的 loopback curl 通道；该通道只允许
  `/v1/session-sync/poll?wait_ms=0..20000` 的查询串，并将同步消息请求上限限制为
  96 KiB。正式设备的 HTTPS 同步请求仍走原来的 `MaiChatHttpClient`。
- 用正常 App、真实测试账号登录流程，模拟器 `AIAssistantUITests/
  testNormalAppReadsAndControlsLocalFakeMacSession` 通过：手机和假 Mac 均上报在线；
  手机读到假 Mac 历史、发送一条文字，假 Mac 收到命令并回传结果和更新的历史。
  本机服务回读显示 `history_request`、`history_page`、`command`、`event` 全部已确认。
  该测试没有真实模型请求、付费工具、腾讯 IM 会话或生产服务。

## 边界

本次是本机协议与正常 iOS 界面的端到端测试。阿里云部署、真实 Mac 宿主到
iOS 真机的跨网络验收、Android 和桌面控制端、媒体按需传输仍未完成。
