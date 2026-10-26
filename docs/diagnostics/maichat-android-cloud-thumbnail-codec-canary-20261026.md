# Android 云盘缩略图生产验收误判与编码限制（2026-10-26）

## 现场与复现

- 平台：`emulator-5554`（Android `sdk_gphone16k_arm64`），正常包 `com.kongshang.maichat`，`2.2.3 (233)`；仅使用一次性测试账号 A、专用私有 OSS Bucket。源码基线 `8ac971d3104ae447f965e8fb6ea0c0d479e4f248` 加工作区未提交改动。
- 初版生产 canary 先上传 Android `Bitmap.compress(PNG)` 生成的 128×80、309 B、SHA-256 `fed604fcf33cf3ce3f9507aec4957684a3d8c9433a25942e6e93464bf589ff12` 图片，再上传 `gallery-sample.mp4`（MPEG-4 Part 2 / `mp4v`，SHA-256 `edc5816d640cea070c8153e557b43bf062882c7b4e15a67e3e3202837a7859e7`）。两项上传、列表均成功；缩略图下载出现 OSS HTTP 400，XML `Code=BadRequest`、`Message=not supported format.`。
- 原测试依次断言图片、视频缩略图，却只输出通用报错，最初把第二项视频失败误归因到 PNG 或 libcurl。

## 定位证据

- 同一失败 URL 的共享 C++ libcurl 与独立 Android `HttpURLConnection` 均返回 400，排除 libcurl 修改请求导致的问题。
- 图片原始 signed GET 回读 SHA-256 与上传源一致，PNG magic 正确，Android 可解码。单独保留图片对象 90 秒后，其解码后的 `x-oss-process=image/resize,m_lfit,w_320,h_320/format,jpg` 在 OSS 公网端点返回 **200**、可解码 JPEG；随后对象自动删除。
- 失败的 `gallery-sample.mp4` 视频轨为 `mpeg4/mp4v`；OSS `video/snapshot` 不支持该编码。图片的 `image/resize` 路径没有失败。

## 修正与验收

- 生产 canary 的视频素材改为 [video-circle-source.mp4](../../MaiChat/android/MaiChat/app/src/androidTest/assets/video-circle-source.mp4)，H.264 / `avc1`、160×90、1 秒、1,683 B，SHA-256 `667f909c3285dc190f6893786843fcc75706914f3966987e4a7c78c8a0ad5c34`；断言增加媒体类型与已解码处理参数，使后续失败能直接指出图片或视频。
- 正常 Android 测试包 `2.2.3 (233)` 经真实 OSS 端到端 **1/1 PASS**：图片与 H.264 视频上传、私有缩略图下载及列表显示、图片全屏预览、视频播放、关闭后临时文件清理、删除，以及最终列表/配额恢复。模拟器截图已人工检查：[云盘列表](assets/maichat-android-cloud-v233-media-list.png)、[图片全屏](assets/maichat-android-cloud-v233-image-preview.png)。不支持截图编码时，列表保留媒体类型占位，文件仍可从“播放”入口打开。
- 旧视频流程在本地 TLS 假 OSS 中单独回归：暂停上传后跨进程续传、列表、播放、删除 **2/2 PASS**。图片本地 TLS 上传/缩略图/全屏/缓存清理 **1/1 PASS**。
- 根线程独立回读测试账号 A：无新增 DB 行；OSS 对象数与 Multipart 数均为 0。账号原有的 6 条删除保留记录仍处于 24 小时保护期，本轮没有增加。

## 产物和边界

- 正常 debug 测试 APK：[MaiChat-android-normal-debug-2.2.3-233.apk](../../MaiChat/android/MaiChat/dist-cloud-image-normal-v233/MaiChat-android-normal-debug-2.2.3-233.apk)，SHA-256 `7e4b6ea9bbf943cac600d3517bea5b4f2d913a769136ea1eef2698e013602dd1`。这是开发签名测试包，未作生产签名或发布。
- 当前修正仍在共享工作区，尚无修复提交号。未在 Android 真机、HEIC 图片或用户的 Dolby Vision/HLG 视频上验证本缩略图路径。OSS 不支持的视频编码继续显示占位图，播放能力另由客户端播放器决定。
