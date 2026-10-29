# MaiChat 移动端实时 ASR V2 与登录 STS 预取（2026-10-30）

## 范围、源码与交付状态

- 用户要求移除 iOS 长期腾讯 ASR 密钥，改为 MaiChat 登录时领取后台签发的 STS 临时凭据；iOS、Android 统一使用腾讯实时语音识别 V2。
- 入口为 iOS/Android 的 IM 与创作中心语音转文字。Desktop 没有此次 ASR 能力，也没有加入桌面 ASR 入口。
- 实施时间：2026-10-29 至 2026-10-30，Asia/Shanghai；开发机为 macOS arm64。
- 源码基线：`b275438b9fc40b299ded6a59c2218ba65a22dfce`，修复位于其后的未提交工作区；最终修复提交由统一提交执行者补充。
- 本记录保存源码、假服务测试及构建证据，不表示已发布。生产部署状态、运行进程版本和最终发布产物由主执行者补充。

## 用户操作与预期结果

1. 通过现有 MaiChat 账号登录，或启动 App 恢复有效登录。账号验证成功后异步预取临时 ASR 凭据；领取失败不阻塞登录、不把登录成功改成失败。
2. 在 IM 或创作中心按住转文字。复用当前账号内存中的凭据，或等待登录预取中的同一个领取任务；随后由客户端直接连接腾讯、采集与发送音频。
3. 取消长按、退出页面、退出账号或重新认证时，旧会话不能启动迟到的录音、提交结果或覆盖新账号状态。每轮 Android 录音生成新的 `voice_id` 与签名 URL。
4. 识别结果可以反复修正此前文字，包括稳定句。松手后等待 V2 最终完成，空的 final 通知保留已收到的全文；10 秒未完成则报错并保留草稿，不把部分文字当成成功发送。

上述完整用户操作尚未连接真实腾讯服务验收；下文明确列出假服务、静态检查与构建已经覆盖的范围。

## 原因与修复依据

- 原 iOS `Speech/SpeechRecognizing.swift` 中的 `TencentASRCredentials` 保存长期凭据，IM、创作中心分别静态注入同一组值。这会把长期凭据带入构建输入。
- 已删除该枚举、两处静态注入和旧桌面代理说明。当前 iOS 识别器只接受现有账号客户端下发的临时凭据，没有手填长期密钥或旧密钥回退入口。移除范围是本次源码与新构建输入；历史提交未改写，历史凭据撤销状态由部署记录补充。
- 旧 CocoaPods `QCloudRealTime 3.1.39` 的正式头文件要求 V1 slice/segment 回调，没有 V2 required raw JSON 回调。仅修改引擎名称不能证明已接入 V2。
- V2 文档要求官方分发包的 `v2/` 目录。已取得真实 V2 包、固定来源及摘要，并删除 iOS 的 V1 slice/segment 文本消费路径。
- 原 stop 后 1 秒以 partial 成功收尾会丢失 V2 稍后返回的修正。两端改为等待明确最终结果，最多等待 10 秒后失败；取消不提交结果。

## 固定的官方 SDK 与协议

| 项目 | 已核对值 |
|---|---|
| iOS 官方接入文档 | <https://cloud.tencent.com/document/product/1093/133355> |
| 官方分发归档 | <https://sdk-1300466766.cos.ap-shanghai.myqcloud.com/asr_sdk/QCloudAsrSDK_iOS_v3.1.40.zip> |
| 归档大小 | 42,477,207 bytes |
| SHA256 | `cb58cbf609f73b4a6a7840a09574ba933a3c54a472ab0f441931774dc3b878f6` |
| 分发包 / V2 SDK 内部版本 | 3.1.40 / `sdk_version.h` 的 v2.0.1 |
| 正式二进制目录 | `v2/sdk/QCloudRealTime.xcframework`、`v2/sdk/VoiceCommon.xcframework` |
| 本地依赖入口 | `MaiChat/ios/MaiChat/Vendor/TencentASRV2/QCloudRealTimeV2.podspec`，Pod 锁定 2.0.1，并校验归档 SHA256 |
| 两端识别参数 | `engine_model_type=16k_zh_en_2.0`、`result_mod=1`、`needvad=1`；不增加说话人分离 UI |
| 后台 STS 官方依赖 | `tencentcloud-sdk-nodejs-sts` 固定 4.1.100 |

iOS 实际 SDK required 回调为 `realTimeRecognizerOnRawResult:jsonData:`。
`QCloudConfig` 实际 STS 构造包含 `appId/secretId/secretKey/token/projectId`；已按归档头文件核对，保留原 SDK 录音、音频队列和会话取消代际。
`DidFinishSession` 的头文件说明包含 SDK stop 超时收尾，因此不作为 partial 成功的依据。
iOS 在唯一初始化处显式调用 `setApiParam("result_mod", value: 1)`；二进制含该参数名不能单独证明其默认值恒为 1。

Android 沿现有 16 kHz 单声道 AAC、`voice_format=16` 与 JNI/libcurl WSS 通道，使用临时 SecretId/SecretKey 在本地签名。
STS token 按腾讯官方 Java SDK 约定放在 `X-TC-Token` 请求头，不加入签名 canonical 或 URL，不写入日志。

两端优先把 `sentences.voice_text_str` 当作权威全文快照；缺少全文时，`sentence_list` 按 ID 排序并整份替换。
V2 单句形态按 `sentence_id` 更新。中间态与稳定态均可被后续全文修正，重复回包不追加重复文字；显式空全文或空列表可清空文字，无正文的 `final=1` 保留最后全文。
其他 `voice_id`、取消后或终态后的结果不再修改当前状态。

## 账号、缓存与服务边界

- 唯一凭据接口为账号认证的 `POST /v1/asr/credential`，请求体为空 JSON，返回字段为 `app_id`、`secret_id`、`secret_key`、`token`、`expires_at`；`app_id` 为数字字符串，过期时间为 Unix 秒。
- 现有服务端长期凭据只用于官方 STS `GetFederationToken` 控制面调用；请求 1800 秒有效期，仅授权 `name/asr:RealtimeRecognition`、资源 `*`。
- 后台单个签发器在进程内共享临时凭据缓存，并发请求合并为一次 STS 调用，到期前 90 秒刷新。过期、无效或意外返回的长期凭据会被拒绝；刷新失败不返回旧证书。
- 新接口响应使用 `Cache-Control: no-store`；独立按 MaiChat 账号限制每分钟 10 次，包含上游失败与并发中的尝试。
- iOS 登录成功保存账号后、有效恢复会话核验后，在 `RemoteIMAppState` 共用 hook 异步预取，使用其已有 `accountAuth`；两处输入栏复用同一客户端的缓存与进行中任务。
- Android 在真实登录完成、有效 IM 凭据恢复及账号会话校验后异步预取，录音线程复用当前账号缓存或同一个 Future。客户端缓存到期前 90 秒更新；退出或重新登录推进认证代际，迟到旧响应不能覆盖新账号凭据。
- 临时凭据只保存在客户端内存，音频与识别响应由 App 和腾讯直传，账号服务不转发音频或识别流。
- 已删除旧 `POST /v1/asr/realtime-url` 正式路由及旧签名服务实现、测试路径；该 HTTP 路径返回 404。旧客户端依赖该接口的语音功能需要升级，部署顺序见 `services/auth/README.md`。

## 已完成的验证及证据

| 验证 | 结果与证明范围 |
|---|---|
| 后台 STS 单元测试 | `node --test asr-credential.test.js`：5/5 通过。使用假 STS 客户端，覆盖缓存复用、并发刷新、失败恢复、到期及永久证书拒绝。日志 `/private/tmp/maichat-auth-sts-unit-test.log`。 |
| 后台 HTTP/账号合同 | `node --test --test-name-pattern=ASR service.test.js server.test.js`：4/4 通过。覆盖 401/200/503、no-store、账号限流及旧路由 404。日志 `/private/tmp/maichat-auth-sts-contract-test.log`。 |
| iOS 无网凭据状态 | `scripts/test-asr-credentials.swift` 直接编译生产 `SpeechRecognizing.swift` 后通过；覆盖响应合同、必需 token、90 秒刷新边界、缓存与账号代际隔离、迟到旧响应和退出清理。 |
| iOS 无网 V2 状态 | `scripts/test-asr-v2.swift` 直接编译生产解析器后通过；覆盖全文优先、稳定句纠正、重复及缩短快照、单句更新、空 final、异流、错误和取消。未模拟真实 SDK 网络或麦克风。 |
| iOS 真实 SDK 接口类型 | 对已安装官方 V2 framework 使用真实 iOS Simulator SDK 的 Swift 6 typecheck 通过，涵盖 STS 构造、required raw 回调、完成回调及显式 `result_mod=1`。 |
| iOS 最终 Debug Simulator 构建 | 主执行者确认增量构建 exit 0，日志 `/private/tmp/maichat-ios141-asr-v2-final-build.log`；产物为 `/Users/hongqingwan/Library/Developer/Xcode/DerivedData/MaiChat-dpmznbhuepzeitfmwzvmmzmcdeql/Build/Products/Debug-iphonesimulator/MaiChat.app`，版本 2.2.4 (141)。编译成功不等于真实识别成功。 |
| iOS 长期密钥产物检查 | 主执行者对上述最新 Debug 产物生成仅含布尔结果的凭证 `/private/tmp/maichat-ios141-secret-removal-check.json`，确认 `old_long_lived_secret_id_absent=true`、`old_long_lived_secret_key_absent=true`；未回传旧值或二进制匹配原文。 |
| Android JVM 定向测试 | 23/23 通过：`RealtimeAsrCredentialCacheTest` 5 项、`RealtimeAsrTicketTest` 4 项、`RealtimeSpeechProtocolTest` 7 项、`AuthTerminalErrorTest` 7 项，XML 中 failures/errors 均为 0；日志 `/private/tmp/maichat-android-sts-unit-20261030.log`。 |
| Android Debug 与测试包 | `assembleDebugAndroidTest` 构建成功，日志 `/private/tmp/maichat-android-sts-debug-test-package-20261030.log`。两个 ABI 的真实 native rebuild/link 均 exit 0。本地假 WSS instrumentation 在隔离的 API 37 arm64 / 16 KB 模拟器通过 1/1，耗时 2.494 秒，覆盖缺失/错误/正确 token、JSON 分帧、AAC 节奏及延迟 final。凭证 `sts-validation-20261030.json` 与 `sts-local-wss-final-20261030.log` 位于本机临时目录 `maichat-android-asr-v2-67v8sn81`，不代表真实腾讯识别通过。 |

以上假客户端和假 WSS 样例不使用真实账号、真实腾讯密钥或付费识别流。正文不保存任何凭据值、密码或签名 URL。

## 尚未验证及待补记录

- 尚未调用真实腾讯 STS 或真实 ASR，未验证正式环境的服务开通、实际临时权限、额度或识别质量。
- 尚未以本次最终产物在 iOS/Android 真机上完成登录预取、长按识别、编辑/发送/取消和账号切换的完整体验验收；没有付费识别请求记录可作为验收依据。
- iOS 已有最终 Debug Simulator 编译证据；正式签名设备产物、实际安装运行版本及真机操作结果另行记录。
- 生产配置只读检查：主执行者已恢复 SSH，并确认现有服务的 ASR 三项长期配置存在且格式有效，未回传值；这不证明已部署新接口或已成功取得真实 STS。
- 生产部署：待主执行者补充部署提交、账号服务进程路径/版本、环境配置完整性、生产接口元数据核验、旧路由 404 及最终发布状态。此处不预先判定已部署或已发布。

## 关联依据

- SDK 归档与接入注意：`MaiChat/ios/MaiChat/Vendor/TencentASRV2/UPSTREAM.md`
- 服务配置、鉴权及上线顺序：`services/auth/README.md`
- 腾讯 V2 WebSocket：<https://cloud.tencent.com/document/product/1093/131127>
- 腾讯 iOS V2 SDK：<https://cloud.tencent.com/document/product/1093/133355>
- 腾讯 STS：<https://cloud.tencent.com/document/product/1312/48195>
- ASR 授权粒度：<https://cloud.tencent.com/document/product/1093/48422>
