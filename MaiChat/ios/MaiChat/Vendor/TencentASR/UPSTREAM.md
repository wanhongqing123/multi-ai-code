# Tencent Cloud standard real-time ASR for iOS

普通 `16k_zh` 引擎使用官方 `standard` SDK，并继续使用账号服务下发的 STS 临时凭据。
模块名仍为 `QCloudRealTime`，宿主直接使用 SDK 合并的 `recognizedText` 替换全文。

- 官方接入文档：https://cloud.tencent.com/document/product/1093/35723
- 官方协议文档：https://cloud.tencent.com/document/product/1093/48982
- 固定归档：https://sdk-1300466766.cos.ap-shanghai.myqcloud.com/asr_sdk/QCloudAsrSDK_iOS_v3.1.40.zip
- 归档大小：42,477,207 bytes
- SHA256：`cb58cbf609f73b4a6a7840a09574ba933a3c54a472ab0f441931774dc3b878f6`
- 分发包与 standard SDK 内部版本：3.1.40
- 正式目录：`standard/sdk/QCloudRealTime.xcframework`、`standard/sdk/VoiceCommon.xcframework`
- 设备架构：arm64、armv7；模拟器：arm64、i386、x86_64
- 核对日期：2026-10-30

归档同时包含同名模块的 `standard` 与 `v2` 二进制。V2 SDK 在复制自定义参数后
无条件写入 `result_mod=1`，普通 `16k_zh` 引擎拒绝该参数，因此不能通过删除宿主的
`setApiParam` 调用来继续使用该二进制。当前 Podspec 只选择 `standard` 目录，并在
CocoaPods 下载时验证归档 SHA256。

标准 SDK 的 required 回调为 slice/segment，`recognizedText` 由 SDK 合并全文，
宿主不重复追加。后台长期密钥不下发，客户端通过带 token 与 projectId 的
`QCloudConfig` 构造鉴权，并直接向腾讯传输音频。宿主在 SDK 明确完成后才提交结果，
10 秒未完成则报错并保留草稿，取消不提交部分文字。
