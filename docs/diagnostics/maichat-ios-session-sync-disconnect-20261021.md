# iOS 设备同步断线提示与远端等待横幅（2026-10-21）

## 现场

- 用户的 iPhone 14 Pro Max / iOS 26.5.2 安装 `com.kongshang.maichat`
  2.2.3 (95)，安装后的可执行文件 SHA-256 为
  `8490adfbc2bfcb9f25dd4158a4aed9839a82e677e940394a0230eb077b56220d`。
  18:28 的 4G 截图在远端 Session 中反复显示红色“设备同步连接中断”；另一张
  截图显示输入栏上方蓝色“等待所属设备确认操作…”横幅，用户要求移除该横幅。
- 上线前账号、设备同步、Nginx 服务均为 active，同步 `/health` 为 200；生产
  同步库曾在读取时显示该 iPhone 最近约 17 秒仍有上报，说明不是持续宕机。
  当时真机随后在 CoreDevice 中变为 `unavailable`，无法继续读取本机诊断日志。

## 定点原因

- 原账号访问令牌只有 15 分钟。`session-sync-server.js` 每次处理 WebSocket 消息
  都用握手时的 `Authorization` 再调用账号服务；客户端刷新令牌后，服务端继续
  校验旧令牌，并关闭连接。C++ 客户端两秒后会自动重连，但 iOS 对每次短暂失败
  都显示八秒红色横幅。安装后约半小时的现场时间也与周期性令牌轮换相符。
  蜂窝网络本身仍可能独立断流；本次没有从不可用的真机抓到运营商侧断线原因。
- iOS `AIAssistantModel` 在远端命令送达后额外设置 `remoteStatus`，视图将它渲染
  为常驻蓝色等待横幅。命令结果、超时和草稿恢复另有独立状态，不依赖该横幅。

## 修复与验证

- 修复提交 `15fc0d4a1c6f0f3f9195e8d26ee623aba28f5ab9`：访问令牌有效期改为
  24 小时；WebSocket 新增 `reauth`，仅当新令牌仍属于同一账号、设备名额与登录
  代次时才在原连接上更新鉴权。自动重连的后台失败不再反复显示红条；主动发送
  失败和命令超时仍显示错误并保留草稿。移除蓝色等待横幅，保留命令回执处理。
  隐私政策的令牌期限与生效日期同步更新。
- 本地账号／同步服务测试 86/86 通过；`reauth` 测试验证旧令牌失效后，原
  WebSocket 用新令牌继续查询，同时拒绝另一账号的令牌。iOS Release 真机架构
  构建成功；正常 App 的 iPhone 17 Pro 模拟器远端 Session UI 用例通过。导出的
  失败后草稿截图已目视查看，蓝色等待横幅消失、原有失败提示和草稿仍可见。
- 生产部署前备份旧服务代码、隐私政策、账号库和设备同步库到
  `/var/lib/maichat-auth/backups/session-sync-reauth-15fc0d4a/`，两个 SQLite 副本
  `quick_check=ok`。部署后账号服务、同步服务、Nginx 均 active；线上
  `service.js`、`session-sync-server.js`、`privacy.html` 的 SHA-256 分别为
  `f1b959d00b4abdc4fc0a3e86a32cefa5ff4b549897921a64cfb4af9af2241f32`、
  `f9e1d06237f031cf466dee53863e21d8ef2bb5b6b507fc2b78e3ac738e7b5391`、
  `ef1fc8d9f5037d8a74cdc12abdfdc4a9a7d617ce541f12701ba2fba30928fd29`。
- 经正式 HTTPS/WSS 和隔离测试账号登录，服务器返回 `expires_in=86400`；刷新
  令牌后，原 WebSocket 的 `reauth` 与目录请求均为 200，连接仍为 OPEN。
  测试账号提交注销后，该连接按预期以 1008 关闭。已清理测试 IM 账号、设备
  同步行和账号库，收据回读 completed，数据库完整性正常；未发送聊天或付费
  模型请求。

## 后续真机验收与边界

- 2.2.3 (96) 真机开发签名包已构建，Bundle ID 仍为 `com.kongshang.maichat`，
  团队 `VLFG56JT33`，可执行文件 SHA-256
  `63d693c04a7af1680971f9b6808b50c85982dcdc7c99deb57b437e8296504aba`。
  iPhone 恢复连接后，于 19:47 原位覆盖安装；真机回读 2.2.3 (96) 并成功启动。
  真机 `testConnectedDeviceShowsProductionSessionDirectory` 通过，启动页和创作
  中心会话抽屉截图已目视查看。测试暂时安装的 Debug 包随后被 Release 96 覆盖，
  最终运行路径为设备上的
  `/private/var/containers/Bundle/Application/C9688D82-00E5-4D87-943D-CDA7F70FAE42/MaiChat.app/MaiChat`。
  生产同步库连续回读该手机最近心跳约 4～18 秒、本机会话 12 个；没有卸载 App
  或清理用户账号数据。
- 真机验收时状态栏为 Wi-Fi，没有亲自在 4G 路径上复现原断线，也没有等待完整
  24 小时令牌周期。此次证明已安装新包、正常启动和短时在线；长期蜂窝网络稳定性
  仍需后续观察。App 的真实网络断流自动重连机制保留，主动操作失败仍有提示。
- App Store Connect 先前上传的 2.2.3 (95) 不包含此修复；若提交审核，应先
  上传新的构建。
