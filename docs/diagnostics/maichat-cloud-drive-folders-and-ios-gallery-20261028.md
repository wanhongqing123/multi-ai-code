# 云盘目录工具与 iOS 图片连续浏览

- 时间与基线：2026-10-28，macOS 开发机，工作树基于 `62b493b5b742975049580a177138c46aed69d941`。普通客户端与工具代码在同一工作树验收；生产部署与真机状态见后文。
- 用户操作：在创作中心通过 Agent 创建、重命名、移动、删除云盘目录，并把已有文件移入目录；iOS、Android 云盘页面保持简单列表，不增加目录管理菜单；Desktop 增加根目录/上一级导航、文件夹创建/改名/移动/删除空目录、文件移动和子目录上传归档。iOS 点击云盘图片后，在同一预览容器向左看下一张、向右看上一张，首尾停止；放大时横拖只平移图片。

## 目录的调用入口和边界

- 普通调用统一经过 `MaiChat/Network/MaiCloudDriveClient.h` 的 `mai_cloud_drive_request`。新增 `LIST_FOLDERS`、`CREATE_FOLDER`、`UPDATE_FOLDER`、`DELETE_FOLDER`、`MOVE_MEDIA`，Desktop、iOS、Android 宿主复用这份 HTTP 核心。
- Agent 对应五个工具：`maichat_cloud_drive_list_folders`、`maichat_cloud_drive_create_folder`、`maichat_cloud_drive_update_folder`、`maichat_cloud_drive_delete_folder`、`maichat_cloud_drive_move_media`。普通代码不需要通过工具协议调用。现有 `maichat_cloud_drive_list_media` 增加可选 `folder_id`。
- HTTP 目录接口为 `GET/POST /v1/cloud-drive/folders`、`PATCH/DELETE /v1/cloud-drive/folders/{id}`；移动已完成文件为 `PATCH /v1/cloud-drive/videos/{id}`，请求 `{folder_id}`。文件列表省略 `folder_id` 保持原来的全量列表，空字符串只列根目录，UUID 只列对应目录直属文件；创建时省略或空 `parent_id` 表示根目录，更新时省略表示不改父目录。
- SQLite 迁移新增 `cloud_drive_folders` 和媒体的可空 `folder_id`；历史媒体归根目录，OSS key、原文件、Multipart、签名读取、配额计算均不改变。所有接口按账号隔离；同级 NFC 名称不能重复，拒非法名称、循环移动和跨账号父目录。只许删除空目录，不递归删除文件。每账号最多 1000 个目录，账号最终注销级联清理目录元数据。
- 会话附件继续使用独立表、OSS 前缀和配额，Session 模式不能调用目录动作。普通 C API 保留根目录空字符串；工具层规范化 UUID，复用名称校验。目录响应必须有有效时间字段，文件移动响应必须为完整的 ready 媒体元数据，畸形成功响应不会被标为正常结果。完全访问模式沿用 `MaiTurnRunner::toolNeedsApproval` 的现有放行策略，目录操作没有另设强制审批。

## iOS 预览的生命周期与已发现问题

- 原页面只打开单张 QuickLook。新增 `MaiCloudDriveImagePager` 使用一个 `UIPageViewController` 连续承载图片页；每页 `UIScrollView` 缩放，放大时停用翻页手势。没有通过销毁旧弹窗再开新弹窗切图。
- 初次点击沿用 `downloadForPreview` 生成的专用临时文件，邻页按需异步下载，解码复用 IM 图片管线和内存缓存。预览最多保留八份下载原件和相邻三张解码页；只删除本次 loader 返回的临时文件，不删除用户源文件或共享素材。失败页显示重试，并可滑回上一张。
- 分页只向图片快照追加新 ID，不重排既有页。未读完元数据时显示“已加载”数量，接近末端可继续取下一批；末个已加载图片仍有后续游标时提供“加载更多图片”。追加发生在手势中时，等待手势结束后刷新邻页，保留当前控制器与缩放状态。
- 模拟器曾稳定复现关闭后临时文件未在五秒内清理：只依赖 SwiftUI 的 `dismantleUIViewController` 会延迟销毁。修正为用户点关闭时立即 `dispose`，取消未完成任务并清理专属文件；正常移除视图和 dismantle 继续兜底。最终连续浏览同例通过，关闭后缓存按预期清理。
- 代码复核另发现后续游标重复时会保留 `nextCursor`，但重试又被 `visitedCursors` 拒绝，导致末页按钮永久无效。已改为检测重复游标时保留可重试游标并报告加载错误；分页追加用例验证第二批图片可继续浏览。右滑容器首次使用 SwiftUI DragGesture 与 Button 冲突，最终用限定触点区域的 UIKit 水平 pan，仅横向开始并取消按钮点击，竖向拖动仍由原 ScrollView 处理。确认弹窗和预览展示期间停用底层 row pan。

## 已有验证证据

- `services/auth` 全套 `npm test`：162/162 通过，包含账号隔离、非法名称、循环、空目录删除、媒体移动、同时间戳分页、旧库迁移重开和账号注销。最终目录定向 9/9 通过；HTTP 路由集成 4/4 通过。日志：`/private/tmp/maichat-cloud-folders-auth-tests.log`、`/private/tmp/maichat-cloud-folders-final-tests.log`。
- root 完成的最终共享 C++ 检查：`mai_cloud_drive_local_tls` 通过（39.09 秒），包含真实 TLS 的目录、根过滤、移动与畸形响应测试；`cloud_drive_agent_tools` 通过（0.36 秒）。工具日志：`/private/tmp/maichat-cloud-folders-tool-final.log`。
- Desktop 使用更新后的 C++ 核心：`cloud_drive_dialog_local_tls`、`cloud_drive_agent_tools_local_tls` 2/2 通过，完整构建与独立 `--smoke` 通过。日志：`/private/tmp/cloud-folder-desktop-latest-network-ctest.log`、`/private/tmp/cloud-folder-desktop-latest-network-smoke.log`。
- iOS 26.5 模拟器最终 4/4、零跳过：目录 Agent/普通 Swift API、图片分页追加、连续浏览失败重试/缩放/首尾边界/缓存清理、真实 TLS 上传图片→整卡点开→右滑揭露删除→确认取消保留→确认删除。结果 `/private/tmp/maichat-cloud-ios-confirmed.xcresult`，耗时 62.85 秒。root 独立回读 summary 并查看最终右滑截图与录屏 frame-136，拖动时两页同时存在。Android 本地 TLS 工具宿主链路最终 1/1（7.17 秒），正常 App 云盘截图没有目录管理入口。
- Windows 本轮没有构建、打包或替换现装 App；Mac 仅验证独立构建路径，未替换用户当前 Desktop App；本轮没有发布 GitHub Release。

## 生产部署与真机状态

- 用户明确回复“现在部署”后，于 2026-10-28 18:45:38 CST 切换账号服务。运行目录 `/opt/maichat/releases/cloud-folders-20261028T104447Z-a09adb09/auth`，PID 118075；新增 systemd `30-cloud-drive-folders.conf` 仅设置 WorkingDirectory。`store.js` SHA-256 `a09adb09d2568dd896127d6110ed830f0c79d3b805a0836f816d0f042bf2a492`，`cloud-drive-service.js` 为 `abe6a277e13a15990c926a1224c5cc50ae754c61c640fbc26cdd47e1c2eafc8c`。
- 先通过 better-sqlite3 在线备份至 `/opt/maichat/backups/cloud-folders-20261028T104447Z-a09adb09/auth.sqlite`，在克隆副本验证新增表/列迁移；未复制活跃 DB 文件或覆盖 live DB。原媒体全部旧列 SHA-256 `b7640056765094eb052194fdcbd817e7909bd0cbf5ccfc463b724763f5e64e62` 在备份、迁移、上线后与 root 独立回读始终一致；quick_check=ok、FK 错误为零。
- 专用测试账号通过正式 HTTPS 创建三个测试目录、列出、改名、移动、拒绝循环和非空删除，随后三个目录清零并退出登录，原 active session 集合恢复。root 独立读生产目录总数为零、原媒体摘要不变；health=200，无凭据 auth/me 与 folders 为预期 401。候选使用现有线上 server/service；未部署之前尚未开放的 Session 附件路由，其路径仍 404。
- 执行命令只请求重启 maichat-auth；现有 maichat-session-sync.service 的 Requires=maichat-auth.service 造成同秒联动重启。Auth/Session Sync 当前健康且近期错误日志为零；media 和 nginx 启动时间保持原值。回滚只需移除本轮 30 drop-in 并重启 auth，默认保留追加 schema；禁止自动恢复整库导致丢失上线后的登录或媒体元数据。
- iPhone 14 Pro Max（iOS 27.2）最新 Debug App 已编译、签名核验并由 Xcode 安装，版本 2.2.4 (131)，MaiChat.debug.dylib SHA-256 `80340aec38a305b9895bead0bb3c1822d5f0b36bcb9a55d1d6f2239e78ce34c2`。首次真实图片只读 UI 测试在初始化前被系统“为 XCTest 输入 iPhone 密码 / Enable UI Automation”阻挡而超时，未执行产品断言；已通知用户在设备自行确认，未索要密码。真机最终滑动验收待此系统确认后续跑。
