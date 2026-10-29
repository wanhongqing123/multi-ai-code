# Alibaba Cloud NUI iOS SDK

当前仅集成 NLS 实时语音识别，保留腾讯语音识别 SDK。移动端输入为 16 kHz、16 位、
单声道 PCM，音频由客户端通过 SDK 直接发送至阿里；账号服务只下发短期 NLS token。

- 官方实时文档：https://help.aliyun.com/zh/isi/user-guide/nui-sdk-for-ios
- 官方下载表：https://help.aliyun.com/zh/isi/sdk-selection-and-download
- 固定包：`V2.7.6-01B-20261026_iOS_TLS.tar.gz`
- URL：https://help-static-aliyun-doc.aliyuncs.com/file-manage-files/zh-CN/20261027/slwtfu/V2.7.6-01B-20261026_iOS_TLS.tar.gz
- 大小：190,911,265 bytes
- 官方 MD5：`c28ca7792b5249aa1454a1a81e3511c3`（下载后已核对）
- SHA256：`d26a8276e386abf44d569f3cd5480d4abaf7de6aebb30a4e5eb16c9389f87207`
- 正式目录：`Release/nuisdk.xcframework`，动态 Framework，最低 iOS 12
- 实际平台：arm64 真机、arm64 模拟器；该 XCFramework 不包含 Intel 模拟器
- 核对日期：2026-10-30

归档没有独立 LICENSE/NOTICE 文件，含厂商集成说明与版本历史；使用遵循阿里云官方
SDK 文档及服务协议。当前不复制厂商 Demo 的录音实现，而使用宿主的 PCM 采集适配。
SDK 控制接口在共用音频工作线程串行调用；事件回调只复制结果和供音，取消与释放
派发到回调之外，避免 SDK 内部死锁。关闭 SDK 文件日志，不保存凭据或调试音频。

`app_key` 与短期 `token` 写入初始化 JSON，开始每轮识别时可通过 `dialogParam`
更新 token。纯云端功能无需 workspace；Token 过期时间以服务返回的 ExpireTime 为准。
