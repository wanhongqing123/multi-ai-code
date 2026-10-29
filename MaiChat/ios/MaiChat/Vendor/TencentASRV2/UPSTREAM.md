# Tencent Cloud ASR V2 for iOS

本地 Podspec 只固定官方二进制来源，不修改腾讯 framework。Swift 继续导入
`QCloudRealTime` 模块；Pod 名 `QCloudRealTimeV2` 用于明确区分 standard SDK。

- 官方接入文档：https://cloud.tencent.com/document/product/1093/133355
- 官方固定归档：https://sdk-1300466766.cos.ap-shanghai.myqcloud.com/asr_sdk/QCloudAsrSDK_iOS_v3.1.40.zip
- 归档大小：42,477,207 bytes
- SHA256：`cb58cbf609f73b4a6a7840a09574ba933a3c54a472ab0f441931774dc3b878f6`
- 分发包版本：3.1.40；V2 `sdk_version.h` 版本：v2.0.1
- 实际引入：`v2/sdk/QCloudRealTime.xcframework`、`v2/sdk/VoiceCommon.xcframework`
- 设备架构：arm64、armv7；模拟器架构：arm64、i386、x86_64
- 核对日期：2026-10-29

官方归档同时包含 `standard/` 与 `v2/`。两者 framework 和模块同名，不能按模块名
判断已升级：`v2` 头文件的 required 回调必须是
`realTimeRecognizerOnRawResult:jsonData:`，不能使用 standard 的 slice/segment 拼接。
CocoaPods 下载时验证上述 SHA256，更新 SDK 时必须同步核对目录、头文件和内部版本。

当前引擎固定 `16k_zh_en_2.0`，不打开说话人分离。账号服务仅下发 STS 临时凭据，
客户端使用带 token 的 `QCloudConfig` 构造并直接向腾讯传输音频。
实际头文件的构造包含 `projectId`，与在线文档的简写不同，接入以归档头文件为准。

V2 文本以原始 JSON 全文快照为准；`realTimeRecognizerDidFinish:result:` 只作为 SDK
明确完成信号，不读取旧版本地拼接文本。`DidFinishSession` 的头文件说明包含 SDK
stop 超时收尾，因此不据此把中间文字报告为最终成功。宿主等待 final/明确完成，
10 秒仍未收到则报错并保留草稿，不发送部分文字。
