# AI 助手移动端接入

## 实现顺序

1. Android 对齐 iOS 消息交互与后台处理：已完成（402bed2a）。
2. Android 字体、Markdown 和消息布局对齐 iOS：已完成（e5115500）。
3. iOS AI 助手：已完成（c5e4781）。
4. Android AI 助手：已完成。

## 共用核心

移动端和 Qt 桌面都在进程内构造 `MaiAgent`，没有本地 HTTP 服务，也没有另写 Swift/Java 工具循环。
`shared/agent/MaiMobileAgent` 只负责 C ABI 与界面数据转换；会话、SQLite、Chat Completions/SSE、工具执行、批准、提问与停止仍由 MaiAgent 实现。

事件回调只在锁内合并增量，不能读数据库或解析 Markdown。移动端后台执行器取快照；纯文本增量复用消息结构，结构事件才重读 SQLite。工作期间界面每 100ms 合并刷新，闲置时每 500ms 检查变化，页面不可见时停止轮询。最后一个事件不依赖界面此前是否认为任务忙碌，因此不会漏掉结束或错误。

## iOS 界面

- 底部增加 AI 助手入口，独立于好友 IM 会话。
- 新建、切换、删除、清空对话；保留会话历史与各对话未发送草稿。
- 流式正文使用现有 IM Markdown 组件及原有样式；异步准备下一段时保留上一段，避免闪空。
- 思考及工具详情可展开；运行中有耗时和停止按钮，结束后可以复制回复。
- 输入区提供文本文件导入、权限、模型配置入口。未配置模型可直接进入设置。
- 支持请求批准、帮我批准、完全访问三种模式。审批卡提供拒绝、单次允许、本会话允许；未知裁决不放行。
- 核心提问显示建议答案和自由输入；用户答复后继续原任务。
- 模型地址与名称可编辑；只接受 HTTPS 配置。API Key 存 Keychain，不写配置 JSON、日志或源码。
- 配置切换需等待当前任务结束；保存失败恢复旧配置。

## 手机能力边界

提供手机沙箱工作区内的读取、写入、编辑、补丁、目录查找、文本搜索，以及网络读取、时间和提问。导入目前限 5MB 以内的 UTF-8 文本/代码；不宣称能分析尚未支持的图片或二进制文件。

iOS 与 Android 不注册桌面 shell，也不伪装成能操作远程电脑。手机后台长期运行、跨设备会话同步、Responses/Anthropic 协议及上下文压缩尚未实现。

## 构建与验证

iOS 在 Xcode 构建阶段使用 CMake 编译仓库内的 MaiAgent、SQLite 与 curl，静态链接进入应用。iOS TLS 使用系统 Security/Secure Transport，保留证书校验，不依赖 Mac 的 OpenSSL。

- MaiAgent 原有 14 组回归通过。
- 移动适配器集成用例覆盖真实本地 SSE 分段、正文/思考分离、忙碌时拒绝重配置、授权前禁止写文件、未知裁决拒绝、批准后执行、中断、429 错误、重启历史、清空和删除。
- iOS Release 模拟器与真机 arm64 构建通过（未做签名安装）。
- iOS Markdown 回归 36 项通过。
- iOS XCTest UI 用例在独立新会话中实际输入、发送、检查最新正文可见、停止下一轮、打开对话列表，通过。

测试服务仅位于 `shared/agent/tests/mobile_model_server.py`，监听本机 18189。模拟器通过 `--ai-ui-test` 使用单独临时目录及测试密钥；真机包不包含这个分支，不接触正式 IM 数据。

```sh
python3 MaiChat/shared/agent/tests/mobile_model_server.py
# 另一个终端：通过 MaiChat scheme 执行 MaiChatUITests。
cmake -S MaiChat/shared/agent -B /tmp/mobile-agent-tests -DMAICHAT_MOBILE_TESTS=ON
cmake --build /tmp/mobile-agent-tests --target MaiMobileAgentTests
ctest --test-dir /tmp/mobile-agent-tests --output-on-failure
```


## Android 界面与后台

新增底部 AI 助手入口，具备与 iOS 相同的会话、模型/权限配置、流式回复、停止、文本导入、授权及提问功能。正文复用 IM 的 Markwon 渲染与第 2 项完成的 iOS 字体参数；思考和工具详情可以展开。最近 50 条 AI 消息先进入列表，用户可按需显示更早消息。

Java 的 `MaiChat-Agent` HandlerThread 负责 JNI、请求/响应 JSON、SQLite、证书导出、密钥读写和导入文件；C++ 使用原有 MaiAgent 工作线程执行网络与工具。列表复用消息行，Markdown 解析使用既有后台队列；输入框不会跟着流式更新重建。页面退出时解除监听，应用内的会话核心保留。

API Key 用 Android Keystore 的 AES-GCM 密钥加密，配置与会话在应用私有 no-backup 目录；包和源码不包含用户配置的模型密钥。切换服务商域名时需重新填写密钥。Android TLS 使用校验过 SHA-256 的 Mbed TLS 源码包，系统根证书分别传入模型请求和 webfetch；不能关闭校验绕过错误。

Android 的测试使用 debug-only Activity 和独立临时工作区。Release 中没有测试 Activity 或测试入口。`mobile_model_server.py` 可通过 `adb reverse tcp:18189 tcp:18189` 供模拟器使用。HTTPS 测试使用无效测试 Key 检查握手后的 HTTP 错误，并确认过期证书被拒绝，不消耗用户模型额度。


### 最终恢复与输入行为

发送请求只禁用发送按钮，保持编辑框及键盘可用；请求确认期间继续输入的新草稿不会被清空。应用重启后，对没有实际运行任务的未完成回复显示“已中断”，保留已生成内容。单纯的历史 `completed=0` 不再被当作仍在思考。


### Android 验证结果

- arm64-v8a、armeabi-v7a 的 Debug 与 Release 构建通过。
- 原有单元测试 113 项通过。
- Android 原生集成用例 3 项通过：AI 对话（中文/emoji、授权写入、停止、键盘展开后的长回复底部定位、编辑框实例保留、Keystore 加密文件）；HTTPS 正常证书与过期证书；既有 IM 历史/键盘/右滑交互回归。
- 共用 MaiAgent 原有 14 组测试、移动适配器集成用例通过，包含进程重启后遗留未完成消息的状态判断。
- APK 仅供本地测试，未发布到 GitHub Release；当前本地构建会读取开发机的 ASR 配置，不能直接作为公开包上传。
- 验证覆盖构建、模拟器与 TLS 实际网络。尚未做手机真机及手机—电脑双端联调。
