# 创作中心出现 Device presence is required（2026-10-27）

## 现场

- 用户 iPhone 截图约 15:01，创作中心欢迎页下方出现红色英文错误
  `Device presence is required`。截图未显示 App 版本，不能据此确认安装包哈希。
- 预期：首次连接和 WebSocket 重连后，跨设备会话能自动登记本机，再读取
  远端历史或发送命令；界面不应出现该内部协议错误。

## 定点证据与原因

- `services/auth/session-sync-server.js` 只在当前 WebSocket 的 `announced=false`
  且收到非 `presence` 操作时返回这句精确英文错误。因此报错表示当前连接
  尚未完成本机在线登记，不能解释为二维码或模型服务问题。
- 基线 `685d952f` 的 iOS `AIAssistantModel.swift` 每 15 秒按
  `lastRemotePresence` 上报，`AIRemoteSessionTransport.request` 则在连接成功后
  直接发历史/命令。WebSocket 新连接的服务端 `announced` 每次从 false 开始，
  客户端计时器却可能仍保留上次登记时间；初次登记未完成时也存在请求竞争。
- 这组客户端时序可以解释截图中的错误。生产手机的具体网络切换时刻与
  运行版本尚未取得，不能把“曾重连”当作截图已证明的事实。

## 修复与回归

- iOS 传输层按 WebSocket 连接代次记录在线登记状态；每次新连接必须先
  完成 `presence`，才放行 `catalog`、`history`、`send` 等操作。断线时清除
  旧登录代次和登记状态，保留最近一次登记内容供重连后安全重发。
- Android 共用相同 WebSocket 服务协议，原实现也沿用 15 秒在线计时。
  连接事件现重置该计时及本机登录代次；其他请求在新 `presence` 成功前
  等待，断线或关闭时解除等待并报错。Desktop 在连接建立时立即主动上报；
  本轮未观察到 Desktop 同样的用户现场。
- 本机隔离 Session 同步 fixture 增加只用于测试的手机 WebSocket 断开入口；
  iOS UI 回归用真实模拟器 App，在假 Mac 在线时强制断线重连，再立即打开
  远端历史，检查内容可见且不出现该英文错误。
- iOS 26.4 / iPhone 17 Pro 模拟器：临时关闭传输层在线登记门槛后，
  **同一 UI 用例失败**（重连后无法读取远端历史）；恢复修复代码后，
  `testRemoteHistoryReannouncesPhoneAfterSocketReconnect` **通过**（27 秒）。
  fixture 记录到 WebSocket 连接代次增加。人工检查成功截图：假 Mac 历史
  正常显示，未出现 `Device presence is required`。截图中另有隔离测试账号的
  腾讯 IM `70402` 提示，与 Session 同步失败分属不同路径。
- 旧功能 `testNormalAppReadsAndControlsLocalFakeMacSession` 24 秒通过：
  打开假 Mac 历史、发送命令及失败后恢复草稿仍正常。测试底栏点击位置
  按 iPhone 17 Pro 的实际图标中心调整，首次尝试误点到了云盘页，未执行
  目标复现；修正测试点击位置后才计入上述验证。
- Android Debug 和 AndroidTest 构建成功；模拟器上的
  `reconnectWaitsForFreshDevicePresence` 1/1 通过，确认新连接重置旧在线状态，
  请求等待本次登记。原有 lifecycle 回执测试也 1/1 通过。真实 iPhone、
  Android 真机及生产服务未改动、未验证。
