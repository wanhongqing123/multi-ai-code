# MaiChat 移动端自有网络请求清单

## 范围与结论

本次范围是 iOS、Android 和两端共用的 MaiAgent 中，**由我们自己的代码发起**的
HTTP(S) 与实时语音 WebSocket 请求。移动端自有请求统一使用项目已有的
libcurl + OpenSSL；应用层仍由 Swift、Java 或 C++ 负责参数、业务状态和界面。
“公共工具”只说明功能入口共用，不代表不同平台的宿主实现原先共用同一个
HTTP 客户端。

| 功能 | 发起请求的实际实现 | 本次结果 |
|---|---|---|
| 手机号账号、会话、临时 IM/模型凭据 | iOS/Android 账号传输桥 | 此前已改为 curl + OpenSSL；本次复核并共用代理处理 |
| Agent 模型调用及其 C++ 工具 | MaiAgent C++ 网络层 | 原本已使用 libcurl；移动端构建选用 OpenSSL |
| IM 联系人头像 | iOS `RemoteIMAvatarImageStore`、Android `AvatarImageLoader` | 改为 curl GET；保留 4 MB 上限及安全的头像跳转 |
| 旧模型服务临时凭据 | iOS `AICloudCredentialSync`、Android `AIAssistantController` | 改为 curl JSON POST；HTTPS、短响应上限、不跳转 |
| Ark 素材服务及 OSS 签名与上传 | 共用 `MaiMobileAgent.cpp` 注册的 `MaiArkAssetTools.cpp` | 正式工具调用原本就走 C++ libcurl；iOS 宿主只提供服务地址与令牌 |
| 腾讯实时语音流 | Android 自写的 `TencentRealtimeSpeechStream` | 自写连接层改为 curl WebSocket；保留 AAC 发送节奏、队列上限和取消 |

旧 iOS 宿主中 `oss_video_upload_config` 和 `mobile_oss_upload_video` 没有对应的
正式工具注册，实际创作请求不会经过那两条 Swift 分支。本次已移除它们及仅为
旧 Swift 文件 PUT 服务的传输能力；`mobile_ark_service_config` 仍保留，供共用 C++
工具获取服务配置。这样不会让同一种素材上传同时存在两套可误用的实现。

Android 原先为了自写实时语音连接显式引入的 OkHttp 依赖随之移除；这不修改
腾讯 SDK 的二进制内容或它可能自行选择的内部网络实现。

腾讯 IM、腾讯语音 SDK、TRTC 等**第三方 SDK 内部**的网络实现不在我们的源码
控制范围内，本次未声称修改。iOS PhotoKit 的 iCloud 原件获取、系统浏览器、
HTML 视图加载的附属资源属于系统组件管理的访问，不是上述自写 HTTP 客户端；
它们没有被静默当成“已改为 curl”。桌面端的 Qt 网络请求也不属于这次移动端
迁移范围。

## 传输边界

- HTTPS/WSS 校验证书链与域名；Android curl 使用当前 Android 信任证书，
  iOS curl 使用 App 内的 CA 包。代理按当前系统设置选择，不能解析的代理不暗中直连。
- 带令牌的 POST 和 C++ 素材上传均不自动重定向。头像 GET 最多跟随 5 次
  同协议跳转；iOS 每跳重新选择系统代理。HTTPS 头像不降级到 HTTP。
- 响应体在下载过程中按调用方上限中止；正式 C++ 素材上传从本地文件逐块读取。
  取消请求时传输层停止读写。Android 实时语音的出站队列上限为 128 KiB。
- 不在日志里记录 Bearer、签名 URL 或用户素材内容；测试只使用本机 HTTP/WSS
  服务与虚构号码，不请求真实短信或付费模型。

## 验证记录

- iOS：Debug 模拟器 App 真实启动；保留的 Swift 桥通过本机 HTTPS GET、JSON POST、
  超限、取消和错误 CA 拒绝验证。头像安全跳转 302→200、HTTPS→HTTP 拒绝及
  超过 5 跳拒绝也已验证。先前旧 Swift PUT 的 128 KiB mock 曾成功，之后发现
  该分支未注册并已移除；这项历史测试不能当作正式 C++ 上传链路的验收。
  Release iphoneos 编译通过。iPhone 14 Pro Max 上用一次性只读入口从 App
  安全存储取当前配置，请求素材服务 `credentials` 的 `action=status`，返回
  HTTP 200、`storage_configured=true`、`assets_configured=true`。这只证明
  手机连通素材服务且服务端配置已开启；该接口不访问 Ark/OSS，不能据此宣称
  Ark 列表、素材创建或文件上传成功。一次性入口随后从源码移除，干净的签名包
  已保留数据重装到 iPhone 并启动，设备显示 MaiChat 0.1.84。
- Android：Debug、Release APK 和测试 APK 构建通过；模拟器本机 HTTP 场景 7/7 通过，
  涵盖跨主机不同代理、仅变更查询串的签名地址以及拒绝跨协议跳转；本机 WSS
  场景 5/5 通过，已有 AAC 录音场景 1/1 通过。最终包保留数据安装到 OP5A47，
  冷启动正常；设备安装包与构建产物的 SHA-256 同为
  `67466620136a6f56a55aafa683c12102bfa41afb72b9b770b3df75a4d3550836`。
  已查看真机登录页截图，未发送短信或调用腾讯付费语音。
- 真正访问 Ark 的只读列表查询，以及任何真实 OSS 文件上传均未执行；前者
  可能涉及供应商计量，后者会写入资源。Android 模拟器与真机均未登录，联系人
  页面没有做端到端头像渲染验收。上述限制不由测试通过替代。

Android 原先验证码 `Connection reset` 的握手分析另见
[`maichat-android-sms-connection-reset-20261016.md`](maichat-android-sms-connection-reset-20261016.md)。
