# 云盘文件改名工具、桌面入口与生产验收

- 记录日期：2026-10-29；本轮构建、宿主验证和生产切换发生于 2026-10-28 晚间（Asia/Shanghai）。
- 客户端工作树基线为 `703b616caa54e57f35f068c050b805349f836bf7`；文件改名核心已提交并推送为 `f3cf40db5c1fcea4dc82e83194bf61c404c3ea33`，iOS 合并验收视图随本记录的提交收录。生产候选仅包含 `services/auth/cloud-drive-service.js`，以本文完整 SHA-256 确认实际运行源码。
- 用户需求：通过 Agent 修改本人云盘已有文件的名称；iOS、Android 不增加文件改名 UI；Desktop 同时提供 Agent 工具和云盘文件行的改名入口。图片、视频都适用，不能借改名转换文件格式。

## 调用入口与行为边界

- 普通入口为 `MaiChat/Network/MaiCloudDriveClient.h` 的 `mai_cloud_drive_request`，新增 `MAI_CLOUD_DRIVE_RENAME_MEDIA = 12`，请求 `{media_id,name}`。非大模型模块直接调用此 API，不经过工具协议。
- 共用工具注册表 `MaiChat/MaiChatTools/MaiCloudDriveTools.cpp` 增加 `maichat_cloud_drive_rename_media`，参数同上。Desktop 的 `DesktopCloudDriveTools.cpp`、iOS 的 `AIMobileHostToolProvider.swift`、Android 的 `MaiCloudDriveAgentTools.java` 均接入该工具。
- iOS 普通 Swift 调用为 `MaiCloudDriveClient.renameMedia(id:name:)`；Android 普通 Java 调用为 `MaiCloudDrive.renameMedia(id,name,completion)`，经 JNI 复用同一 C++ HTTP 核心。移动端此次没有添加文件改名按钮、菜单或弹窗。
- 服务端复用 `PATCH /v1/cloud-drive/videos/{id}`，文件改名请求仅传 `{name}`，只允许所有者更新 `ready` 文件。该 PATCH 也支持显式传入 `folder_id` 与名称一起原子更新；专门的 rename 工具不携带目录位置。
- 文件名按 NFC 规范化，必须是安全文件名且不超过 240 UTF-8 字节；拒绝路径分隔符、控制字符、未配对 surrogate、系统保留名及非法后缀形式。已有扩展名必须保持，比较忽略大小写，例如 `.mp4` 改为 `.MP4` 合法，改为 `.mov` 返回 400。文件 MIME 类型也必须保持。
- 对于 rename-only 请求，除 `name` 外的 ID、账号归属、目录、OSS object key、大小、内容类型、上传和清理状态等字段不变；原始字节、签名读取和配额不变。独立 Session 附件空间不开放该操作。
- C++ 对成功响应继续校验完整 `ready` 媒体元数据和扩展名，不把畸形成功响应视为完成。工具沿用现有 Agent 权限策略，Desktop 将服务端非法改名的 HTTP 400 映射为 `InvalidInput`。

## Desktop 实际操作

1. 进入“我的云盘”，在已有图片或视频行点击“改名”。
2. 沿用 `AppTextInputDialog`，预填完整旧名，并提示保留原扩展名。
3. 取消时保留原文件；空名、过长、非法名称或改变扩展名时显示错误，保留原文件行和当前目录。
4. 成功后刷新当前目录显示新名；使用原 ID 继续播放、预览、移动和删除。

普通 UI 通过 C API 进行改名，和 Agent 工具复用核心。实际 Cocoa 窗口已走通取消、错误后缀拒绝、中文视频名 `演示视频.MP4`、图片改名，以及改名后的播放、图片预览、移动、删除和原有上传/续传/账号切换流程。已亲看 740px 和 540px 窄窗截图，新文件名和播放、保存、改名、移动、删除按钮没有重叠。

截图目录为 `/private/tmp/maichat-cloud-rename-cocoa/`，关键文件为 `cloud-file-rename-dialog.png`、`cloud-file-renamed.png`、`cloud-folders-narrow.png`。

## 构建和宿主验证

| 层级 | 已有结果与证明范围 |
|---|---|
| Auth 后端 | 全套测试 167/167、失败 0；覆盖名称验证、扩展名保持、账号隔离、仅 ready 文件、原子更新和原元数据保持。日志 `/private/tmp/maichat-cloud-rename-auth-tests.log`。 |
| 共享 Network | `mai_cloud_drive_local_tls` 1/1 PASS，38.01 秒；普通 C 请求经实际 TLS 走改名和列表更新、错误后缀拒绝及响应校验。日志 `/private/tmp/maichat-cloud-rename-network-test.log`。 |
| 共用工具层 | `cloud_drive_agent_tools` 1/1 PASS，1.22 秒；工具注册、参数和调用边界回归。日志 `/private/tmp/maichat-cloud-rename-tools-test.log`。 |
| iOS 宿主 | 模拟器 `testCloudDriveFolderAgentTools` 1/1 PASS，46.37 秒。Agent 改名后普通 Swift list 读取新名和同一目录；普通 rename 后 Agent list 读取同一新名；允许 MP4 扩展名大小写变化，拒绝改成 MOV。结果 `/private/tmp/maichat-cloud-rename-ios-host.xcresult`。 |
| Android 宿主 | 普通 Java/JNI 与 Agent 链路 1/1 PASS，38.69 秒；同样验证两种入口交叉读取新名、目录保持、扩展名大小写与错误格式拒绝。日志 `/private/tmp/maichat-cloud-rename-android-host.log`；Debug/AndroidTest 构建成功。 |
| Mac Desktop | `maichat`、`cloud_drive_dialog_test`、`cloud_drive_agent_tools_integration_test` 重链成功。两项 CTest 2/2 PASS：UI 3.66 秒、工具 8.93 秒；Cocoa 可见窗口的同一完整 UI 流程另行通过，截图已人工查看。 |

Desktop 日志为 `/private/tmp/cloud-rename-desktop-{final-build,cocoa,ui-ctest,agent-ctest,smoke}.log`。构建目录中独立 App 的 `--smoke` 退出 0，日志核对实际运行路径为 `MaiChat/desktop/build-screenshot-mac/MaiChat.app/Contents/MacOS/MaiChat`，版本 2.2.31、Qt 5.15.18、macOS 27.2 arm64、OpenSSL 3.5.9。

## 生产单文件发布

沿本会话用户明确“现在部署”的云盘服务授权，在 root 确认手机 UI 诊断已停止、可发布后，于 **2026-10-28 23:56:21 CST（15:56:21Z）** 切换。SSH 使用已有可信 IP 主机密钥并强制严格校验，未新增 known_hosts 或接受未知密钥，未打印凭据。

- 旧运行目录：`/opt/maichat/releases/cloud-folders-20261028T104447Z-a09adb09/auth`。
- 新独立 release：`/opt/maichat/releases/cloud-rename-20261028T155551Z-e321be48/auth`。复制当前运行包，保留 `auth/node_modules` 与同级 `shared` 的已有绝对链接，只替换 `cloud-drive-service.js`；没有覆盖历史 release。
- 新增 `/etc/systemd/system/maichat-auth.service.d/40-cloud-drive-rename.conf`，内容仅 `[Service]` 与新 `WorkingDirectory`；保留 30 及更早 drop-in、原环境文件和相对 `ExecStart`。
- 切换前通过 better-sqlite3 的在线备份生成 `/opt/maichat/backups/cloud-rename-20261028T155551Z-e321be48/auth.sqlite`。同目录保留旧源码、main unit、drop-in 目录和 `migration-evidence.json`。未直接复制活跃数据库文件，未覆盖 live DB；本次没有 schema 变更。

| 对象 | SHA-256 |
|---|---|
| 候选及线上 `cloud-drive-service.js` | `e321be48600a7441bfeff601a3d91e39985e7f4bf3c54a1430ed863d3665bffa` |
| 保持不变的线上 `store.js` | `a09adb09d2568dd896127d6110ed830f0c79d3b805a0836f816d0f042bf2a492` |
| 单文件候选 `.tgz` | `38130ed0973791a827c8e43bd29803e5937bb17d4c91af9297ec1403c343c097` |
| 备份与上线后相同的 schema 定义 | `58389bbcf874cbaa13896ff9b295fc0c8cc0e67eb16bcf3095b8f21eb57135f3` |
| 原有 26 行云盘媒体全部字段摘要 | `c2ddf802400cf4ec56e511a0cafcecd2d94f7df1d73fab63d21d4cb187cd0c65` |

执行命令只请求重启 `maichat-auth.service`，其已有依赖关系 `maichat-session-sync.service Requires=maichat-auth.service` 会联动重启 Session Sync。最终 Auth PID 为 118879、Session Sync PID 为 118881，均在 23:56:21 启动。15:59:49Z 回读两者健康接口均为 200，近期错误日志为零，实际 cwd 和两文件 SHA 与候选一致。Media PID 111444 与 Nginx PID 27457 的启动时间仍分别是 10 月 26 日和 10 月 8 日，没有重启或修改其代码和配置。

root 随后独立回读生产源码哈希、服务健康和 canary 证据，均通过。

## 正式 HTTPS 与真实 OSS canary

使用已有 root-only 专用测试账号凭据，在空闲设备名额登录，避免替换该测试账号已有活跃会话；凭据和 token 仅在进程内使用，没有输出。验收素材为本地产生的 **79 字节、16×16 纯色 PNG**，SHA-256 为 `87ea892f9c987d06325235d29dd670372f041a600f65a311e793053ec58ba5df`，没有使用用户文件、付费模型或伪造数据库记录。

请求在 ECS 将 `ichat.life` 解析至 loopback，保留正式 hostname、SNI 和证书校验，经过现用 Nginx HTTPS 路由；OSS 分片 PUT 与原件 GET 则使用实际签名 HTTPS 地址直连私有桶。

最终成功轮于 **2026-10-28 15:58:38Z** 完成：

1. 经正式 `https://ichat.life/api` 路由进行普通 Multipart 初始化、分片签名，向真实 OSS PUT，读取权威 ListParts 后 Complete。
2. 请求改为中文名称和 `.PNG` 后缀；通过普通媒体列表读回新名，验证 ID、目录、大小、类型及除名称外的数据库字段保持不变。
3. 获取原对象 `play-link`，按已有 OSS 验收 helper 校验签名目标、实际下载字节数和 SHA-256；原对象 key 与内容保持一致。
4. 请求改为 `.jpg`，服务返回 400，名称未改变；再还原原始 `.png` 文件名。
5. 仅删除本次专用测试 ID，核对元数据行为零、该精确对象 key 的 Multipart ID 为零、原签名对象 GET 返回 404、配额回到初始值。
6. 退出专用测试登录，原活跃 session ID 集合恢复。原有 26 行媒体全部字段摘要在备份、上线、canary 清理后始终一致；数据库 `quick_check=ok`、外键错误为零。

首轮 operator 验收脚本在独立清理检查中误把既有内部 `list-uploads` 响应字段读为 `uploads`，实际契约为 `upload_ids`，因此脚本失败。该错误不在生产产品源码：首轮测试对象已通过正常 DELETE 清理，测试登录也已退出。修正临时脚本字段后，先独立核对首轮精确对象不存在、Multipart 为零、元数据为零、session 已失效及原媒体摘要不变，再将恢复 journal 归档为 resolved；随后完整重跑上述成功轮。期间没有再次重启服务，没有绕过清理租约或直接修改数据库。

生产证据为 `/root/maichat-cloud-rename-20261028T155551Z-e321be48/rename-canary-evidence.json`；没有未处理的 recovery journal。本机日志为 `/private/tmp/cloud-rename-production-{deploy,canary-final,readback}.log`，候选清单为 `/private/tmp/maichat-cloud-rename-auth-candidate.json`。临时部署与 canary 脚本在 `/private/tmp/maichat-cloud-rename-deploy/`，OSS PUT/GET 校验函数摘取自已有受测验收脚本并保存来源 SHA。

## 回滚与尚未交付的边界

- 代码回滚移除本轮新增 `40-cloud-drive-rename.conf`，`daemon-reload` 后重启 Auth，已有 30 drop-in 会恢复旧文件夹版本目录；随后同时检查 Auth 与联动 Session Sync 的健康。部署脚本包含该失败回滚路径；本次成功发布没有实际执行回滚。
- 禁止自动恢复整库快照，避免覆盖上线后的真实登录或媒体元数据；本次结构未变，没有恢复数据库的需要。
- 本轮 Windows 没有重新构建或实机复验，不能用 Mac 结果代替 Windows 验收。
- Mac Desktop 只验收独立构建产物，用户正在运行的 Desktop App 未替换，也没有为本轮发布 GitHub Release。
- iOS rename 普通 API 和 Agent 宿主已在模拟器通过，但本轮真机 rename 工具尚未实测。不能宣称包含最终 rename、ScreenCapture 和目录浏览修正的合并 iOS App 已安装交付；相关真机与合包工作仍由对应任务继续完成。
