# 多端 Session WebSocket 切换与上线记录（2026-10-20）

## 需求与实现

用户要求把各端每约 15 秒拉取 Session 目录和长轮询消息，改为 WebSocket 主动推送；
所属设备继续执行自己的 MaiAgent、工具和模型调用。用户建议把 WebSocket++ 和 Boost
放在 `MaiAgent/third_party`，以便四端共用客户端代码。实现选择 WebSocket++ 0.8.2
配独立 Asio 1.30.2，省去整个 Boost；两份上游头文件树、许可与源码归属记录均已入库。
共享 C++ WSS 客户端在 `MaiChat/SessionSync`，Qt、Swift、Java 分别接入登录态、CA、
系统代理和界面。服务端只路由同账号设备消息，仍不代理模型或媒体供应商请求。

源码提交：`b6e6efaacd25285337b6dd4ac2fed039153a6c4f`。

## 本地验证

- 服务端 `npm test`：80/80 通过。WSS 测试覆盖未登录拒绝、账号隔离、目录立即推送、
  消息送达与去重、收件确认、离线通知及断线后未确认只读消息重投。
- MaiAgent 八套 CTest：8/8 通过。Mac Qt 主程序和 Session 宿主编译通过；
  `desktop_session_sync_host_test` 通过；`login_dialog_test` 的新凭据入口定向用例通过。
- iOS 正常 App 的模拟器 UI 测试两次通过：登录本机测试账号，看到假 Mac 会话，
  打开所属设备历史，发送无付费文字指令，拒绝时保留草稿。导出并目视查看了历史和
  拒绝页面截图。本机假账号的 IM 证书会出现单独 70402 提示，未将其记为 Session
  WSS 结果。
- Android Debug APK 的 arm64 与 armeabi-v7a 原生构建、Debug/Release Java、
  146 项单测通过。本机 Android 37 模拟器通过 JNI 到带测试 CA 的 WSS 实际握手；
  远端会话打开失败保留草稿的设备测试也通过。随后用无活跃会话的已导入测试账号，
  同一模拟器经正式系统 CA 和生产域名完成 JNI/WSS 登录握手 1/1；测试账号随即
  退出为 200。未操作 house 的 OPPO 真机，也未在 Android 模拟器发送模型请求。
- Windows 从同一源码用 VS2019/Qt 5.15.2 增量重链主程序及相关测试成功；
  Session 宿主与登录测试 CTest 2/2 通过。现装 Windows App 未替换。

## 生产部署和回读

- 在原阿里云 ECS 上备份 `session-sync-server.js`、`session-sync-service.js`、
  `package.json`、`package-lock.json` 和 Nginx 配置，备份后缀为
  `before-websocket-b6e6efa`。确认现有 `ws` 为 8.22.0，部署两份 JS 与锁定依赖，
  新增精确 `/api/v1/session-sync/ws` Upgrade 路由。`nginx -t` 通过后只重启
  `maichat-session-sync` 并热重载 Nginx；`maichat-auth` 没有重启。
- `maichat-auth`、`maichat-session-sync`、`nginx` 均为 active；本机服务 `/health`
  为 200。公网 Nginx 新 WSS 无凭据为 401，旧 HTTP Session 路由无凭据仍为 401，
  `/api/v1/auth/me` 无凭据仍为 401。
- 选取原已导入且无活跃会话的测试账号，经过生产 Nginx TLS 路由完成有效 Bearer
  WebSocket 101 握手与空 Session `presence` 200；随后账号退出为 200，回读该
  测试账号活跃会话数为 0。没有发送模型指令或付费请求。Mac 经公网路由也确认
  无凭据 WSS 401。旧 HTTP 路由在已发布客户端升级期保留。

| 线上文件 | SHA-256 |
|---|---|
| `session-sync-server.js` | `4b44ee75cfcfe63a1e327f4e95fd656624c27baca1cf0f053705b81ba4a97503` |
| `session-sync-service.js` | `d509390c787e1d4207ce337d82dfd605034a979ab3e515d3ae0640946c8f8f9d` |
| `package.json` | `ae944aed2929b8158a293afc120ab71dfa58ada95fe30882a0bb90474ce21ead` |
| `package-lock.json` | `96da4536f77a1a0e18266f1e991d318a832f87dd2b018066b6b02a5bc229ca6e` |
| Nginx `ichat.conf` | `2aa829d7d960eaa17fad6df80e75ad318f83085cd9496a36afa12f51bed9f9ca` |

## 客户端版本与真机回归

- [Desktop 2.2.6](https://github.com/wanhongqing123/multi-ai-code/releases/tag/maichat-desktop-2.2.6)
  从产品源码 `83c87c5ba97291d71c6a1ee1a14e67a8d2f5a876` 发布为 Latest。
  Mac DMG、Windows EXE/ZIP 的公开资产大小与 SHA-256 均和本地记录相同；
  Mac 共用 C++ 客户端经生产公网 WSS 实际完成有效账号 101 和空目录上报 200，
  随后退出。Windows 的 OpenSSL curl 直连无凭据路由 3/3 返回 401；同机系统
  Schannel curl 在 TLS 阶段 reset。Windows 产品使用内嵌 OpenSSL；正式 Windows
  App 的有效账号跨设备操作还未实测。两端现用安装目录没有被替换。
- [Android 2.2.3](https://github.com/wanhongqing123/multi-ai-code/releases/tag/maichat-android-2.2.3)
  的最终签名 APK 来自 `2397b385e04ae03fbf594bc057def2c333722363`，
  159805729 B、SHA-256
  `8032b4333c0cb976be95cee695a0b18bc5656502efcd1f4ddf7cf82013f194eb`。
  OPPO PKD130 已保留数据原位安装，设备回拉 `base.apk` 哈希一致；生产 WSS
  假 Mac 目录和历史可读。首次候选发现展开侧栏视图不会随推送刷新，修复后
  同一真机保持侧栏展开，上线原位出现、清理原位消失、草稿保留，见
  `docs/diagnostics/maichat-android-session-drawer-live-20261020.md`。
- 两个 GitHub Release 都附 `MaiChat-SessionSync-ThirdPartyLicenses.txt`，
  8880 B、SHA-256
  `b619ca34d2fec98799d4f87fb1096bbdd7f80ee52899046661a089587ac0a56a`，
  完整重现 WebSocket++ 与独立 Asio 的许可文本。
- iOS 2.2.3 (91) 已在模拟器通过正常 App 的假 Mac 历史与无付费文字流程，
  并用用户设备的开发描述文件签成真机 App；本地开发 IPA SHA-256
  `ec7b59d758c08b7d92132fc49326a4aaef9fbb9120f3fe55eaa2b5725d60990c`。
  `codesign --verify --deep --strict` 通过。iPhone 在 Xcode 中持续为
  `unavailable`，因此尚未安装或做 iOS 真机生产 WSS 验收；该私有设备描述文件
  不适合公开发布，且有效期至 2026-10-24。

## 尚未验证

生产环境尚未由升级后的两台真实产品设备对同一个真实所属设备 Session 完成
查看、发操作、执行结果的全链路验收；Android 真机读到的是无付费假 Mac 的
固定历史。iOS 真机安装等待设备重新连接。跨设备媒体、离线历史缓存等原方案
未完成项亦未因改用 WebSocket 而自动具备。旧客户端在过渡期仍使用服务端
保留的 HTTPS 接口。
