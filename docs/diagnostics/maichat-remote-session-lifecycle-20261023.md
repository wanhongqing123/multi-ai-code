# 跨设备新建与删除 Agent Session（2026-10-23）

- 需求：同一 MaiChat 账号的 iOS、Android、Mac、Windows 设备可以在另一台设备上新建和删除 Session；由所属设备在自己的 Agent 存储中真正执行。本轮源码基点为 `8ac971d3104ae447f965e8fb6ea0c0d479e4f248` 加工作树改动。
- 协议：同步目录的 `catalog.devices` 包含零 Session 的设备。调用方使用 `send` 给当前登录代次的在线目标发送 `session_id=session_lifecycle` 命令：`session_create` 可带所属设备上已存在的绝对 `workspace_path`，省略则用目标默认 Workspace；`session_delete` 带 `target_session_id`。目标返回同一保留 Session ID 的直接 `command_result` 事件，并在本地创建或删除后上报 presence。服务端只做账号、槽位、登录代次、目标 Session 与结果关联校验，不创建本地 Agent Session，也不代理模型请求。命令首次投递后不自动重放；`send` 的 200 不是执行成功。
- 失败用例与修复：原服务要求每个 `send` 的 Session ID 已在目录中，因此对零 Session 设备新建返回 404；原 Desktop Host 先查已有 Session，同样拒绝生命周期命令。服务与各端增加保留命令路由后，原失败用例转绿。生命周期结果必须直接发送，不能用普通消息的分块事件格式；服务端严格校验对应原命令的收据。iOS、Android、Desktop 的入口都只在目标设备回复成功后更新结果；远端删除有确认。
- 本地验证：服务端 Session 与 WebSocket 定向测试 16/16；Desktop Host、Channel、MainWindow 三组 CTest 3/3，Mac Release 构建与独立启动检查通过；Android Debug APK 与测试 APK 构建通过，模拟器远端设备选择、所属设备创建/删除和删除确认 3/3，通过真画面截图核对；iOS 正常模拟器 App 用本机假 Mac 和真实本地 WSS 双向验证了 iPhone→Mac 与 Mac→iPhone 的创建、目录更新和删除。iOS 抽屉的新增入口、远端删除确认及 Mac 端零 Session 入口截图已亲看。模拟器假账号出现的腾讯 IM 70402 提示与 Session 生命周期无关。
- 线上部署：先在 ECS `/var/lib/maichat-session-sync/backups/lifecycle-20261023-Z6st5m/` 保存旧服务文件及 SQLite 在线一致性副本；副本 `quick_check=ok`，设备 6、Session 2、群消息 800。更新 `session-sync-server.js` 和 `session-sync-service.js`，SHA-256 分别为 `cd1869c1069e69a66851d31d904f7659a8cd02af9233cfd957d14edb9ee01dd9`、`cd7168ce56ff94dcf98dd8e652f31ae5fef2cc4c917d9685c6524efec08ce56a`；生产 `maichat-session-sync` 为 active，本机 `/health` 为 200，库 `quick_check=ok`，经生产 Nginx 无凭据目录为预期 401。
- 本地/设备产物：iPhone 已原位安装并启动 MaiChat `2.2.3 (110)`，主可执行文件 SHA-256 `da9b50f35c17acdc19fc934d9263fa7eedd1ed6202351383d285ac6f731ac510`；Android 模拟器已原位安装 `2.2.3 (225)` Debug APK，SHA-256 `ebdff6a8a70d9a8c2261e7f94b2057e1d56b097556b97a8cffb0ac01b7c7466e`；Mac 独立测试包为 `MaiChat/desktop/dist-session-lifecycle-v2216/MaiChat-macos-arm64/MaiChat.app`，版本 2.2.16，主可执行文件 SHA-256 `e6291ca5e96d8ade35de8a399af79f09f60494a90e029035216b1a748795f2c0`，深度签名与独立 `--smoke` 通过。
- 尚未验证：用户当前 Mac 仍运行旧测试包 2.2.14，未中断或替换。真实 iPhone→Mac 创建/删除必须在 Mac 切换到 2.2.16 后验收。Windows 原生包和 Android 真机本轮未安装；iOS 真机 UI 操作也未亲自点击新建/删除。不能据此宣称真实四端互控全部完成。

## 2026-10-24 续：默认 Workspace、切换工具与真实设备边界

- 用户修订交互：远端设备标题的「＋」直接请求 `session_create`，省略 `workspace_path`，由所属设备使用默认 Workspace；不再弹出路径输入框。Agent 工具 `maichat_remote_session_create` 仍可显式给目标设备上已存在的绝对路径。
- 新增 `session_set_workspace` 本地 Agent 工具与 `maichat_remote_session_set_workspace` 跨设备 Agent 工具，均逐次批准。前者在当前 Agent Session 执行时更新同一 Session 的存储记录，从下一轮任务起使用新目录；后者发送 `session_lifecycle` 中的 `session_set_workspace` 命令，由所属 Desktop 验证目标 Session 与绝对现有目录后执行，iOS/Android 所属设备拒绝任意目录。服务端仅校验、转发和关联结果。
- 发布中的服务器在 `/var/lib/maichat-session-sync/backups/workspace-switch-20261023T155543Z/` 做了 JS 与 SQLite 在线备份，`quick_check=ok`；新 `session-sync-service.js` SHA-256 为 `e4a923759e8fa69318edf096f76d5e01eaf76eab5d6e544153dadbfe544232fd`，服务与公开未授权边界分别返回 200/401。服务端针对本扩展的 HTTP+WS 用例 16/16 通过。
- Mac 2.2.18 独立测试包：`MaiChat/desktop/dist-session-lifecycle-v2218/MaiChat-macos-arm64/MaiChat.app`，主程序 SHA-256 `aecb0c248981ce930f7e7dce3fbedbbae6e5a5697523dd5acdf1f038560853ab`。Host 22/22、Channel 5/5、Layout 144/144、MaiAgent 相关套件 8/8，通过包内 `--smoke` 且 SSL 可用。2026-10-24 00:01 本机从此包启动，进程路径与版本核对；服务器后续看到 Mac 心跳恢复。
- iPhone 2.2.3 (112) Release 编译及覆盖安装成功，主程序 SHA-256 `26b86c30b4e9d74c669317c589c59fb7b0fb8dd2f5921d7782b729e3e2553b1c`；随后指定真机 UI 测试构建了同版本 Debug。模拟器本机假 Mac WSS 的工作目录 Agent 工具测试通过：批准前无切换，批准后回执和目标目录均正确；远控 Mac Agent 审批卡在 iPhone 显示并以统一 `approved` 协议值回传的测试通过。
- Android 2.2.3 (226) Debug 安装到模拟器，APK SHA-256 `cdb3a51e0f39a7292b6f7c67598adf3d8ef968b0c8a64d3e3b08dd4255b52f2d`，所装文件哈希一致；定向模拟器测试 4/4 通过。无 Android 真机及 Windows 新包验收。
- 真机 UI 自动化在进入实际生命周期操作前受 iPhone 当前界面/横竖屏与测试导航影响，未完成真实 iPhone↔Mac 创建删除。第二次测试中用户明确要求停止操作手机，已立即终止测试进程；之后不再安装、启动或查询手机。真机上最后可能运行的是测试框架安装的 Debug (112)；不能声称 Release (112) 仍是当前安装产物。两台设备曾在服务端目录同时出现在线心跳，但这只证明连接，不证明跨设备操作已验收。

## 2026-10-24 模拟器测试账号验收

- 用户改为只用模拟器和测试账号。真机上此前已重新覆盖安装并启动 Release (112)，此后停止所有真机操作。当前 Mac 用户实例继续使用原账号；没有切换或覆盖它的数据。
- 本机 `session-sync-ui-fixture.js` 使用内存账号、HTTP/WSS 与假模型；Mac 槽位由 JS 假设备提供，不是完整 Desktop App。iPhone 17 Pro 模拟器从最新移动端源码重建，远端手动创建/取消删除/确认删除、假 Mac→iPhone 所属设备创建删除、Agent 远端创建、Agent 远端删除逐次批准、Agent 远端工作空间切换逐次批准、iPhone 批准远端 Mac Agent 请求，六个定向 UI 用例各 1/1 通过。检查了真实测试截图：手动创建没有路径框，删除前出现本地破坏性确认；假测试账号会出现独立的腾讯 IM 70402 提示，不能将此提示归因于 Session 同步。
- 移动端本地 `session_set_workspace` 曾允许更改 MaiAgent Session 目录，但宿主相册/音乐等仍指向固定默认 Workspace。为避免同一会话出现两个目录，现仅 Desktop 注册本地切换工具；iOS/Android 继续固定默认 Workspace，保留操作远端 Desktop Session 的工具。MaiAgent Core、MobileAgentAdapter、Desktop Host 定向测试通过；Android Debug APK 从修订源码重建并安装到模拟器，所装 APK SHA-256 `09b3c6d45c49624ecd2394d2a61cfb17f8bc1a9aade6a2a3752e22a887f15932`，Android 定向模拟器用例 4/4 通过。
- 验收边界：本地夹具中的 Mac 是模拟 owner；Desktop 真实 Host/UI 由 Qt 定向测试覆盖，但没有在同一运行实例里与 iOS 模拟器联通。用户要求停止真机操作，因此不宣称物理 iPhone↔Mac 端到端成功。线上服务端部署和 Mac 测试包启动状态如上，未因模拟器测试改动。
