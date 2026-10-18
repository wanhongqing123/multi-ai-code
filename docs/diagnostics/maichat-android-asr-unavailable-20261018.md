# Android 按住转文字不可用（2026-10-18）

## 复现与现场

- 设备：OPPO A3x 5G，Android 15，MaiChat 已发布签名包 2.2.1 (221)。
- 操作：登录后进入创作中心，按住“可按住转文字”说话并松手。
- 预期：显示实时/最终转写，松手后按界面操作处理文本。
- 现场：创作中心没有识别结果；设备查询 `android.speech.RecognitionService` 返回 `No services found`。

## 根因证据

- 2.2.1 Release 的 Gradle 配置把 `TENCENT_ASR_APP_ID`、`SECRET_ID`、`SECRET_KEY` 设为空。
- `MainActivity` 据此建立的旧一句话识别器 `isAvailable()` 为 false。
- `VoiceRecordingController` 没有服务端签发入口时不启动腾讯实时 WSS，录音完成后只交给旧一句话识别器；创作中心因此走“没有识别到文字”的分支。
- iOS 使用腾讯实时识别 SDK，在长按期间开始识别、松手后等待最终结果；Android 已有录音/AAC 与 libcurl WSS 实现，缺的是发布包可用的鉴权入口。

## 修复与边界

- 账号服务提交 `c4e9c17e711bb0492e2ee1a6e4658e9a0fbc27b7` 部署 `POST /api/v1/asr/realtime-url`：当前账号 Bearer 换一次性短期腾讯 WSS URL；音频仍由客户端直连腾讯。
- Android 开始录音后在后台领取 URL，校验腾讯主机、`voice_id`、AAC 格式与有效期，并用该 `voice_id` 解析实时结果。松手先于签发完成时等待签发，再发送已录好的 AAC；取消时丢弃迟到结果。
- Android 构建已移除 `local.properties` 腾讯长期密钥注入、旧客户端签名和一句话 SDK；生成的三份 BuildConfig 均没有 ASR 密钥字段。普通 IM 录音仍按已发布包行为回退为语音消息。
- 本地 JDK 17 下 Debug/Release Java 编译、Android 测试源码编译成功，43 份单测报告共 136 项、0 失败；真机新包的账号 200、WSS 与实际转写须在发布签名候选包安装后记录，不能用旧 2.2.1 的现象代替验收。
- 不在日志或诊断记录中存储账号令牌、签名 URL 或原始音频。
