# iOS 阿里云与腾讯云语音识别切换

## 需求与实现

- 2026-10-30，用户要求在 iOS 设置中选择腾讯云或阿里云语音转文字，先完成此功能再处理摄像头自动拍摄。
- 设置入口为「我 → 设置 → 语音 → 语音识别服务」，默认腾讯云。每次长按时固定所选服务，设置变更从下一次识别生效；IM 与创作中心复用同一入口和会话管理。
- 腾讯使用此前已安装并验证过的普通实时识别；阿里使用官方 NUI iOS 2.7.6-01B SDK 和实时语音识别。后台只下发短期 Token，音频由手机直接发送阿里云。
- 两家凭据各自内存缓存，复用账号登录代际、提前 90 秒刷新和迟到请求隔离。登录时预取所选服务；修改选择后下一次长按可按需领取。
- 阿里采集 16 kHz、16-bit、单声道 PCM，SDK 拉取缓存。SDK 操作与音频会话在工作队列串行执行，控制和释放不在 SDK 回调内同步执行；识别任务完成后返回文本，超时或取消不提交部分结果。

## 账号与 SDK

- 官方 SDK 来源、大小、SHA256、架构和文档见 `MaiChat/ios/MaiChat/Vendor/AliyunNUI/UPSTREAM.md`。CocoaPods 下载使用固定 SHA256；包的 XCFramework 包含 arm64 真机及 arm64 模拟器分支。
- 账号服务新增已登录用户接口 `POST /v1/asr/aliyun-credential`，响应为 `app_key`、`token`、`expires_at`；到期时间沿用阿里云 `ExpireTime`，不假定固定时长。腾讯接口保留，音频不经过账号服务。
- 后台配置专用 `MAICHAT_NLS_APP_KEY`、`MAICHAT_NLS_ACCESS_KEY_ID`、`MAICHAT_NLS_ACCESS_KEY_SECRET`；不存在或上游失败时阿里入口返回 503，不影响腾讯或账号服务启动。值不写进源码、日志或本记录。
- 已创建独立 RAM 用户 `maichat-nls-token`，绑定自定义策略 `MaiChatNlsTokenCreateToken`，仅允许 `nls:CreateToken`、资源 `*`；专用配置位于服务器 `/etc/maichat/nls-auth.env`，root 所有且权限 0600。原短信和 OSS 身份权限未改。无密钥操作与回滚凭证 `/private/tmp/maichat-private-deploy-recovery/nls-ram-operation-receipt.json`。

## 验证与边界

- 后台 Token 与 HTTP 定向测试 18/18 通过；实际官方 `CreateToken` 成功，返回过 36 小时有效期，验证不能硬编码 24 小时或腾讯 STS 时长。
- 用户提供的 AppKey 与专用 RAM Token 完成真实 NLS 握手；一次 2 秒合成普通话音频返回中间句、完整“你好，测试成功。”及任务完成，所有状态成功。音频已删除，证据 `/private/tmp/maichat-private-deploy-recovery/aliyun-nls-functional-audio-result.json`。此验证使用真实阿里服务但不代表 iOS SDK 录音已验收。
- iOS SDK 在真实已安装头文件下通过 Swift 6 类型检查；纯状态及 PCM FIFO fixture、两家凭据刷新及账号隔离 fixture 通过。
- 正常 iOS Simulator App 的 `testSpeechProviderSelectionSurvivesRelaunch` 1/1 通过，使用隔离假账号，验证默认腾讯、选阿里、重启后保留和切回腾讯；执行者查看设置页截图 `/private/tmp/maichat-ios143-settings-all-attachments/64A44A2C-AD00-48B5-9EAE-55132866FBC4.png`。
- 生产账号服务更新、iPhone 143 安装启动和实际录音链路状态将在交付时补充；本地 fixture 不作为真实麦克风或生产 Token 领取的证明。
