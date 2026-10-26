# iOS Agent 云盘播放关闭后缓存未删除

时间：2026-10-26，iPhone Air 模拟器，iOS 26.5。源码位于主工作树；检查时当前
提交以 `git rev-parse HEAD` 为准，未把本轮改动提交。运行产物来自
`/tmp/maichat-ios-cloud-agent-cache-canary/Build/Products/Debug-iphonesimulator/MaiChat.app`。

## 复现与证据

在本机回环 TLS 云盘夹具中登录 `sync-ui-test`，上传样本，调用
`maichat_cloud_drive_play_video`，随后关闭 Agent 视频呈现。旧调用链把完整视频下载到
`tmp/mai-cloud-drive-playback/<UUID>.mp4`，只把
`RemoteIMAppState.agentVideoPresentation` 置空；下载文件仍存在。普通云盘页面的播放
关闭回调会删除自己的下载文件，Agent 工具没有相应所有权或清理回调。

定向 UI 回归新增“关闭后缓存应不存在”断言后，旧实现下
`testCloudDriveHostPlayDelete` 在第 61 行失败；本机夹具的上传状态为 `ready`。
这只证明旧调用链未删除缓存。后续复用同一个未重置的夹具曾产生 `not_found`，使用
全新夹具后消失，未把它归因于缓存修复。

## 修复

- Agent 播放单独下载到 `tmp/mai-cloud-drive-agent-playback/`；普通页面的缓存路径不变。
- `MaiCloudDriveAgentPlaybackCache` 仅接管该目录中 UUID 命名的 Agent 文件。
  视频呈现被关闭或替换时删除活动文件；账号变化时关闭该 Agent 视频并删除文件。
- 下载期间账号变化会拒绝呈现，并立即删除刚下载的文件；下载失败由客户端删除目标。
- App 启动时清理该专用目录的完整文件及崩溃留下的 `.maitmp-` 部分文件。
  其他目录中的用户原视频不参与清理。

## 验证

- `testCloudDriveAgentPlaybackCacheLifecycle`：关闭、切号、重启孤儿文件清理和原文件
  保留，1/1 PASS。
- 全新本机 TLS 夹具下 `testCloudDriveHostPlayDelete`：上传、Agent 播放、关闭缓存、
  删除，1/1 PASS。
- 专用 iPhone Air 模拟器使用真实 A/B 灰度账号：A 普通界面上传可播放 MP4 后删除，
  A Agent 工具上传/播放/删除，B 入口隐藏与 API 404，4/4 PASS。两次 A 流程后分别
  回读 A 列表为空、已用和预留字节均为 0。

签名 Release `2.2.3 (122)` 已覆盖安装在 iPhone 14 Pro Max，设备回读版本正确，
启动进程存活，原账号设置仍在，本地消息数据库 `quick_check=ok` 且保有 19,832 行。
用户当时正在使用手机，未在真机调用云盘工具或播放云盘视频；真机视觉与缓存关闭路径
仍未验证。iOS 端没有重复传输 2 GB 文件；共用 C++ 核心的 2 GB 真实 OSS 续传有
独立验收记录。
