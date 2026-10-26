# MaiChat 三端共用媒体下载（2026-10-24）

## 边界

普通 HTTPS 文件传输由 `MaiChat/Network/MaiCurlDownload.{h,cpp}` 唯一实现。桌面、iOS、Android 的腾讯 IM SDK 只获取带签名的图片、语音、文件、视频和封面 URL；下载后各端按自身消息模型更新界面。不同平台的证书、系统代理、沙盒路径和后台调度留在薄适配层。UI 与后台任务直接调用此能力，不经 Agent 工具协议。

共用核心输入原始 URL、目标路径、最大字节数、代理和 CA 选项、取消句柄。它只接受 HTTPS，逐跳校验重定向并可重新选择代理，验证 TLS 证书与主机名，流式写入同目录私有临时文件；只有完整成功才原子发布目标文件。取消、超限、HTTP/TLS/磁盘错误均不留下新目标文件或损坏旧目标文件。签名查询串不重写、不写日志。

## 平台接入

| 平台 | SDK URL 获取与宿主职责 | 共用调用 |
|---|---|---|
| Desktop | Qt/TIM SDK 返回 URL；Qt 提供系统代理、应用缓存路径、账号切换与 UI 投递 | `DesktopCurlAttachmentDownload` 包装同一 C ABI，在后台线程运行 |
| iOS | 腾讯 SDK 取得 URL；Swift 提供沙盒路径、CA、代理和账号代次 | `MaiChatAttachmentDownloader` 调用同一 C ABI |
| Android | 腾讯 SDK 取得 URL；Java/JNI 提供应用私有路径、CA、代理和账号代次 | `MaiChatAttachmentDownloadJni` 调用同一 C ABI |

按媒体种类由宿主传入上限：图片 32 MiB、语音 64 MiB、文件 512 MiB、视频 1 GiB、视频封面 16 MiB。头像是 4 MiB 内存响应，当前各端使用已有的有界 libcurl 路径；这类小响应与大文件流式落盘的 API 形状不同。

## 验收证据和边界

- 共用核心的本地 TLS 测试覆盖签名 URL、HTTPS 跳转与逐跳代理、证书错误、HTTP 错误、超限、取消和旧文件保留；系统 libcurl 与桌面捆绑 OpenSSL libcurl 均通过。
- Desktop 独立附件和 SDK 回调测试 2/2 CTest 通过；Mac 2.2.20 完整重链、独立包签名及 `--smoke` 通过。
- iOS 模拟器本地 TLS 测试 1/1 通过，覆盖图片/文件、取消、旧账号延迟 URL；Android 模拟器 JNI TLS 3/3、SDK URL/回调 JVM 6/6 通过。
- 现有本地测试账号不能登录真实腾讯 IM，故本轮没有两名真实测试账号之间的全媒体收发验收。Windows 原生编译/运行也尚未在本机验证；不能把 Mac 的编译结果当作 Windows 验收。

本轮产物：Mac 独立测试 App `MaiChat/desktop/dist-shared-curl-v2220/MaiChat-macos-arm64/MaiChat.app` 2.2.20，主程序 SHA-256 `0ed733fc2a29e83c2272a40aee7dd8be3f729aab4c09c2d3140f9ed3f55bb3ad`；iOS 模拟器 2.2.3 (113)，Debug 代码 dylib SHA-256 `58cae6e29e2a21f096861f97945406ae4307d484d507c7860364d09c4af7bc2c`；Android 模拟器 2.2.3 (227)，APK SHA-256 `5b75e035bf9072a98a20a63dc04af515d2d519823fb7519dfc7f4e69a3d7d06b`。当前登录中的 Mac 用户 App 仍为 2.2.19；未切换到 2.2.20。
