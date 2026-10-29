# 模型流保活、协议阶段与有效进展（2026-10-29）

## 现场与复现

- 用户在 Desktop 代码审查后看到 `model stream inactive`，而另发“你好”又有回复；原请求与后续请求是两次独立请求。现场版本、时序、工具结果完整性见 `maichat-desktop-review-model-stream-inactive-20261029.md`。
- 旧逻辑只有正文、推理、工具参数增长才重置 120 秒时钟。供应商仍发送 SSE 心跳或 `response.in_progress` 时，也可能被当成完全静默取消。
- 本次以 localhost 假 HTTP/SSE 复现，不发真实模型请求：等待期间持续发送注释保活、重复阶段、空增量、正常阶段及正常正文，分别检查流存活、结束原因和请求次数。

## 上游参照与边界

1. 本地 Codex 子模块 `2165b1da63527b66f4e491ff5e5c96d763eab973` 的 `codex-rs/codex-api/src/sse/responses.rs` 对 `eventsource().next()` 设置空闲期限；完整 SSE 事件先结束本次等待，再按业务类型处理。被忽略的 `response.in_progress` 仍属于一个事件，注释心跳不由这个循环直接感知。
2. DeepSeek 官方 Responses 文档列出 created、in_progress、output item/content part、增量和终态。生命周期存在不等于已完成，也不能由客户端证明服务端正在执行 GPU 推理。官方限流说明中的保活也可能发生在排队等待时。
3. DeepSeek Harness 固定提交 `22ddee3792402c3e70ec5f86a63c3e3e895a2f60`：原生 DeepSeek 适配器走 Messages API；SSE 解析器在完整 frame 和 comment 上调用活动 pulse，再由翻译器忽略合法未知事件。不能将它称为其 Responses 实现。pi-ai 适配器对翻译后的 StreamChunk 等待，两者并非同一层。
4. Harness 两条适配器默认空闲期限均为 300000ms，原生上述路径没有语义进展上限。MaiChat 本轮的 900 秒进展期限是产品策略，并非照搬上游或协议保证；公开 C++ 配置允许设为 0 关闭。

主要资料：

- https://api-docs.deepseek.com/guides/responses_api/
- https://api-docs.deepseek.com/quick_start/rate_limit/
- https://github.com/deepseek-ai/deepseek-harness/blob/22ddee3792402c3e70ec5f86a63c3e3e895a2f60/packages/llm/llm-deepseek/src/sse.ts
- https://github.com/deepseek-ai/deepseek-harness/blob/22ddee3792402c3e70ec5f86a63c3e3e895a2f60/packages/llm/llm-deepseek/src/adapter.ts
- https://github.com/deepseek-ai/deepseek-harness/blob/22ddee3792402c3e70ec5f86a63c3e3e895a2f60/packages/llm/llm-pi-ai/src/adapter.ts
- https://github.com/deepseek-ai/deepseek-harness/blob/22ddee3792402c3e70ec5f86a63c3e3e895a2f60/packages/util/timeout/src/index.ts

## 修复

- Responses 与 Chat Completions 共用同样的双时钟策略。默认连续 120 秒无真实通信才按传输静默取消；收到头/正文（含保活、半帧）及实际上传增长重置通信时钟。只记响应头到达时间，不保存头内容。
- 另一滑动时钟默认 900 秒：非空正文、推理、工具参数增长，以及首次出现的有效协议阶段推进会重新计时。实际上传增长也重置此时钟，避免完整输入尚未传完就误报模型停滞。持续有进展的长输出不受这个滑动时钟截断。
- 有界去重 created/in_progress、item/part/summary、参数完成和搜索阶段；重复阶段、空 delta、注释及未知事件仅证明通信。协议元数据不伪装成助手正文/思考，不写入对话内容，不因状态帧改变原有输出后重试边界。
- 仍须收到 completed/incomplete/failed 等真实终态，不能将 in_progress 或单个 item done 当成整轮完成。区分传输静默和长时间无进展的错误；取消优先级、原瞬时网络重试条件、已输出内容不重试规则保持原有行为，不自动重放工具。
- 同时修复两个客户端原 curl XFERINFO 回调少一个参数的 ABI 错误。正式签名是 userdata 后跟四个 `curl_off_t`，最后一个才是实际上传字节；回调本身被周期调用不算进展。
- `progressTimeoutSeconds` 放在 `MaiModelConfig` 末尾，保留已有聚合初始化的字段位置。iOS、Android、Desktop 均复用此 C++ 实现。

## 验证

- macOS 27.2，`/private/tmp/maichat-pdf-core-gate`：构建八个核心测试目标成功，`MaiCoreTests`、`MaiFilePathTests`、`MaiStoreTests`、`MaiModelClientTests`、`MaiTurnRunnerTests`、`MaiToolTests`、`MaiEditToolTests`、`MaiApplyPatchToolTests` **8/8 PASS，24.83 秒**。
- `MaiModelClientTests` 23.63 秒：两种 wire 均覆盖真实静默、不重复重试、保活后继续返回正文、只有保活最终触发无进展上限；Responses 另覆盖重复阶段/空增量不能无限续期，及多种新阶段使滑动期限持续更新而不产生虚假正文。既有取消、终态、图像输入、重试和长正文测试同时通过。
- 日志：`/private/tmp/maichat-stream-core-build.log`、`/private/tmp/maichat-stream-core-ctest.log`。测试使用缩短的秒级配置检验边界，没有让真实请求等待 15 分钟。
- 本次未修改模型直连架构，也未新增供应商状态查询接口。仅有保活时无法判断内部是否还在推理；只承认能观察到的通信和协议进展。

## 构建交付

源码修复与 Desktop 展示修复统一构建为 Desktop 2.2.32、iOS 2.2.4 (140)。精确修复提交、签名产物、安装与运行核对在完成后追加；在此之前不宣称用户正在运行修复版。

### 完成记录

- 精确修复提交：`6e97705d4d2bafd203a523dae3e8d4ab1a1a6388`，已推送 `origin/main`。全部源码、产品版本与上述测试所用修复一致；未提交改动仅为本文后续验收记录。
- iOS 模拟器：iPhone 17 Pro / iOS 26.5，正常 App 2.2.4 (140)，启动 PID 20709；看过启动截图及完整 17.19 秒录屏的逐段联系图。首次建立隔离测试目录后进入消息页，假 IM 凭据显示已知 70402 错误，不能将此算作真实 IM 通过。未再次重复改名/密码 UI 回归，本轮只改共享模型流。截图 `/private/tmp/maichat-ios140-stream-startup.png`，录屏 `/private/tmp/maichat-ios140-stream-startup.mp4`。
- iOS 真机：iPhone 14 Pro Max / iOS 27.2，普通无参数启动；安装信息独立回读 **2.2.4 (140)**，实际运行 PID **6853**，路径 `/private/var/containers/Bundle/Application/AB4A4E93-FEC1-4A05-B0E4-956AF7193BF7/MaiChat.app/MaiChat`。运行日志 `2026-10-29T13:43:26.148Z app_version=2.2.4 build=140 ios=27.2 launch=aa40f14a pid=6853`；构建签名 `codesign --verify --deep --strict` 通过，二进制中两种新错误均存在。
- iOS 主程序 SHA256 `82621c937625474393d6d58b5ca65c1e39461e35890960da025117844a96c6d0`，Debug dylib SHA256 `098bb469eb2b3029a7d51a24c8c08343d44304006aa8900fa6cc91fec434fb99`。构建、候选、安装、Apps 回读、启动、运行日志证据在 `/private/tmp/maichat-ios140-stream-*`。
- Mac：独立 staging `/Users/hongqingwan/OpenSource/multi-ai-code/MaiChat/desktop/dist/review-macos-20261029/MaiChat-macos-arm64/MaiChat.app`，**2.2.32**，主程序 62124288 B，SHA256 `fcda1b0186456ab12c6703877cc95e045df815559945d56d816c265eb7e2644d`。临时签名及 `codesign --verify --deep --strict` 通过；移除外部 Qt/DYLD 路径、裸 PATH 包内 `--smoke` **exit 0，2.556 秒**。日志回读 Qt 5.15.18、OpenSSL 3.5.9、实际 staging 执行路径及 PID 57181。QPDF LICENSE/NOTICE 随包存在。此检查只覆盖 macOS 27.2 ARM64，构建中现有 Graphics dylib 的最低系统版本高于主目标告警不构成本次旧系统验证。
- Mac 当前 `/Applications/MaiChat.app` 仍为 2.2.31 / `3446aefa`、PID 13707。按仓库对运行中任务的约定，已询问用户何时退出并替换；确认前不重启其正在使用的程序。构建与独立 staging 通过不等于现装版本已更新。
- Android：隔离 Debug **2.2.5 (237)** / `com.kongshang.maichat.isolated`，armeabi-v7a 与 arm64-v8a 两 ABI 重新编译本轮两个客户端并链接，assembleDebug 成功。APK SHA256 `9cbc1d7603aa81093901edcabd12c32f0d5f49ab0f187839a9fd2441ed43e66f`。源文件 SHA 与精确提交匹配；日志 `/tmp/maichat-android-stream-clocks-20261029.log`，完整产物清单 `/tmp/maichat-android-stream-clocks-20261029-artifacts.json`。当前 `adb devices -l` 没有连接设备，本轮未安装、启动或做 Android UI 验证。
- 未跑真实供应商长时间推理、未产生付费调用；未构建 Windows、未发布新版 release。已验证的是隔离 SSE 边界、源码链接及上述平台构建/安装状态，不能由此推断供应商内部工作状态。
