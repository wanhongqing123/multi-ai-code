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

版本 **13.4.21067**，已在 macOS 上展开并完整入库：

```
macos/TXLiteAVSDK_TRTC_Mac/
  ├── TXLiteAVSDK_TRTC_Mac.xcframework
  ├── TXFFmpeg.xcframework
  ├── TXSoundTouch.xcframework
  └── dSYMs/
```

三个 xcframework 都包含 arm64 + x86_64 通用二进制。framework 内部依赖
`Headers -> Versions/Current/Headers` 等符号链接，因此 SDK 必须在 macOS 上展开
后提交，Git 索引以 symlink 模式保存这些链接。解压目录已完整入库，原始
`TXLiteAVSDK_TRTC_Mac.tar.bz2` 归档不再保留。其他 macOS 环境拉取仓库后可直接
获得完整 SDK，无需手动解压；Windows 构建不使用此目录。
