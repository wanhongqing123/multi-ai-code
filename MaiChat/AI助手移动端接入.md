# AI 助手移动端接入

## 实现顺序

1. Android 对齐 iOS 消息交互与后台处理：已完成（402bed2a）。
2. Android 字体、Markdown 和消息布局对齐 iOS：已完成（e5115500）。
3. iOS AI 助手：已完成。
4. Android AI 助手：下一项，复用同一适配器。

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
