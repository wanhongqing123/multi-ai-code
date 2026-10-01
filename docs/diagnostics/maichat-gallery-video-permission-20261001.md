# MaiChat 相册视频导出与权限工具（2026-10-01）

## 现场与复现

- iPhone 用户确认系统相册最新项目中有视频；Agent 的 `mobile_list_photos` 看不到媒体类型，`mobile_export_photo_original` 只得到 PNG/JPEG 画面，随后用 ffprobe/ffmpeg 读取导出路径失败。
- 任务要求 Agent 能主动请求所需系统权限；MaiChat 第一次从消息或 AI 助手入口打开相册时也应触发系统授权。

## 定位证据

- iOS 相册列表使用 `PHAsset.fetchAssets(with: .image)` 并过滤 `mediaType == .image`；原件导出只调用 `requestImageDataAndOrientation`。因此视频不会作为视频出现在列表或导出结果中。
- Android 只查询 `MediaStore.Images`，只申请图片读取权限，原件导入只接受 `image/*`；`loadThumbnail` 返回的是封面 JPEG，并非视频文件。
- ffmpeg 工具直接把模型给出的相对路径传给进程内 FFmpeg，而 Agent 的工作目录并非进程当前目录；ffprobe 将临时 JSON 写到访问边界 `fileAccessRoot`，没有使用明确可写的工作目录。现场的 `Operation not permitted` 没有独立设备日志证明唯一原因，不能把上述路径问题写成已证实的全部根因。

## 修复与验证

- `38ee09d138ed9ced6b26096da6099ebee92f82d4`：两端列表都返回 `mediaType`，新增 `mobile_export_media_original` 并保留旧工具名兼容。iOS 用 Photos 原始视频资源导出 `.mov/.mp4`，Android 从视频 MediaStore URI 流式复制本体。
- 同一提交新增 `mobile_request_permission`，支持相册、相机、麦克风、定位、联系人、日历、通知；系统拒绝时返回状态与是否需要进入设置。Android 的消息和 AI 助手相册入口首次使用时主动请求图片与视频权限；iOS 两个相册入口原已在打开前调用 Photos 授权。
- ffprobe 临时输出改在 Agent 工作目录；ffmpeg 对相对输入与输出文件路径按工作目录解析。桌面进程内 FFmpeg/FFprobe 定向测试通过，包括工作目录路径回归。
- iOS Release 模拟器构建通过。Android Pixel 9 模拟器实测：视频出现在列表，`mediaType=video`，导出文件为 MP4 本体且文件头为 `ftyp`；权限工具返回相册与定位的实际已授权状态。Android 仪表测试通过。

## 未验证边界

- iOS 真机上的 Photos 原件下载与导出尚未实测；iCloud 中仅在线的视频也需真机验证。
- Android 拒绝授权后的系统弹窗与“去设置”路径尚未做自动化交互测试。
- 现场原设备的 ffprobe/ffmpeg `Operation not permitted` 尚未回放到同一文件；当前测试只证明工作目录路径问题已修复，不等于所有沙盒失败都已排除。
