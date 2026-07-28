# tencent-trtc — 腾讯 TRTC C++ SDK（远程桌面用）

腾讯云官网正式版，非本机 liteav 自建产物。

## 下载地址

官方文档页与 GitHub 仓库 README 里写的链接**已失效**（404）。实际有效地址各平台
模板不一致，Windows/Android 带 `/v1/` 段，Mac/iOS 不带：

```
Windows  https://liteav.sdk.qcloud.com/download/latest/v1/TXLiteAVSDK_TRTC_Win_latest.zip
Mac      https://liteav.sdk.qcloud.com/download/latest/TXLiteAVSDK_TRTC_Mac_latest.tar.bz2
iOS      https://liteav.sdk.qcloud.com/download/latest/TXLiteAVSDK_TRTC_iOS_latest.zip
```

链接是从 `https://cloud.tencent.com/document/product/647/32689` 的页面 HTML 里
提取的（该页正文由 JS 渲染，直接 curl 抓不到）。

## windows/

版本 **13.4.0.18168**，取 zip 内 `SDK/CPlusPlus/Win64`，已解包入库：

```
windows/
  ├── include/        19 个头文件
  └── lib/x64/        liteav.lib · liteav.dll · txffmpeg.dll
                      liteav_screen.dll（屏幕采集）· txsoundtouch.dll
```

CMake 通过 `MAICHAT_TRTC_AVAILABLE` 自动接入，构建后把 4 个 DLL 拷到 exe 同目录。

## macos/

**以原始 tar.bz2 归档形式入库**，未解包。

原因：Mac SDK 是 xcframework，内部依赖符号链接
（`Framework/Headers -> Versions/Current/Headers` 等）。在 Windows 上解包会丢失
这些链接，framework 随即失效；重新打包同样丢。保留归档是唯一能在 Windows 检出
的仓库里安全携带它的方式。

Mac 侧构建前先解包：

```bash
cd MaiChat/desktop/vendor/tencent-trtc/macos
tar -xjf TXLiteAVSDK_TRTC_Mac.tar.bz2
# 得到 TXLiteAVSDK_TRTC_Mac/{TXLiteAVSDK_TRTC_Mac,TXFFmpeg,TXSoundTouch}.xcframework
```

包含 arm64 + x86_64 通用二进制，另含 dSYMs 调试符号（构建不需要，可自行删除以省空间）。

解包产物已在 .gitignore 中忽略，避免误把丢了符号链接的目录提交上来。
