# iOS Session WebSocket 冷启动恢复登录（2026-10-20）

## 现场与纠正

用户要求同账号的 iPhone 登录后即建立 WebSocket；iOS 被系统终止后，下次恢复登录
应重新连接，后台不需要强行保活。iPhone 上已保留数据安装 MaiChat 2.2.3 (91)，
`devicectl` 回读 bundle ID `com.kongshang.maichat` 与运行进程路径均为新包。

排障时曾误用账号库内部 `users.id` 查询同步库，得出“无 iPhone 心跳”的错误结论。
同步服务以 `/v1/auth/me` 的**对外 MaiChat ID**为 `account_id`；按用户个人资料
截图中的 MaiChat ID 重查，真机在创作中心前台已在线，最近上报约 5–11 秒，
服务器目录有 12 个本机会话。用户侧栏也显示了服务器返回的远端离线目录。
因此 2.2.3 (91) 的前台 WSS 与目录上报是正常的，先前离线报告不能作证据。

## 冷启动遗漏

`RemoteIMAppState.finishAccountLogin()` 在新登录成功后调用
`AIAssistantModel.startSessionSyncForAccount()`；但 App 冷启动从 Keychain
恢复已有账号时，`MaiChatBootstrap.load()` 只创建并保存 AppState，没有调用
该入口。此前只有进入创作中心后 `AIAssistantModel.appear()` 才补启动同步。
此外，原同步循环等待本机 Agent `ready` 才建立 WSS，导致账号已登录但本机
工作区读取较慢或失败时其他设备仍看不到这台手机。

修复提交 `1528f0eee27a0bdcc2dc0467044d44bb70ed5c9a`：恢复账号后立即
启动同步；新登录时先启动 WSS，再异步准备本机会话。准备完成前先上报空的
本机会话目录并接收同账号其他设备目录，准备完成后立即重新上报本机目录。
退出或切换账号仍停止旧连接；进后台被系统挂起后，重新启动 App 会走恢复
登录入口，再建立新连接。

## 已验证与待验证

- iOS 模拟器正常 App 的本机 WSS 假 Mac 历史/无付费文字测试，在修复后重新通过。
- 独立模拟器使用生产测试账号、正式域名和生产 WSS，正常 App 只读看到假 Mac
  在线 Session 与固定历史，测试 1/1 通过并通过界面「切换账号」退出；图片已在
  Xcode 测试结果中目视核对。无模型调用。该测试使用 Debug 模拟器临时内存刷新
  令牌，规避无签名模拟器的 Keychain `-34018`；真机和 Release 均不走该分支。
- iOS 2.2.3 (92) 真机 App 已用原设备开发证书签名，`codesign --verify --deep`
  通过；iPhone 在安装前再次显示 `unavailable`，故尚未原位安装，也未对
  **冷启动后停留在 IM 页即在线**做真机验收。设备描述文件有效期至 2026-10-24。

不把模拟器通过或 2.2.3 (91) 的创作中心前台连接等同于 2.2.3 (92) 的
真机冷启动恢复验收。
