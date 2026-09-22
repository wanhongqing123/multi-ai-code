# MaiAgent 与 Codex 交互链路审查

审查基线：`bf99cda1d229`。目标是对齐 Codex 的交互原则和安全边界，不复制 CodexApp 的 Markdown 外观；MaiChat 的 Markdown 继续由 IM 与 AI 助手共用。

## 当前链路

1. `AgentChatPanel` 接收用户输入，只负责界面状态。
2. `AgentController` 把 Qt 调用翻译为 `MaiAgent::submit()`，并把工作线程事件排队送回主线程。
3. `MaiAgent` 创建会话、落库用户消息和 assistant 占位消息，并为每轮任务启动独立工作线程。
4. `MaiTurnRunner` 通过 `MaiContextBuilder` 组装完整历史，调用 `MaiModelClient`，把正文、推理和工具调用保存为不同 part。
5. 工具调用先确定审批策略，再执行并把结果回灌给模型；模型可以继续调用工具，最多 12 轮。
6. `AgentController` 根据 part 类型发出正文、思考、工具、权限、提问和结束信号，`AgentChatPanel` 增量更新同一条会话流。

这条边界是合理的：模型网络、数据库和工具执行不在 Qt 主线程；Qt 主线程只接收排队事件、查询已落库状态并更新界面。

## 本轮已修复

- 模型配置可从设置页录入，API Key 输入被遮挡，保存后无需重启；“未配置模型”可直接点击配置，未配置时发送也会打开配置入口。
- 思考过程改为轻量可折叠状态，运行时显示“正在思考”，完成后显示耗时。
- 工具调用默认显示一行摘要，命令与输出按需展开；展示层截断长输出，不修改核心保存的数据。
- 三档审批策略接入核心：请求批准、帮我批准、完全访问。运行中的任务不能中途切换策略。
- 收紧 Shell 免审批白名单。`find -delete`、`env`、`tree -o`、`diff --output`、`rg --pre`、`date -s`、`hostname` 和可触发外部 diff/textconv 的 Git 命令都会请求批准。
- HTTP 错误保留供应商返回的 message/code；429 单独标记为 `rate_limited`；瞬时连接失败和 5xx 只在尚未产生流式输出时重试，避免重复回答。
- 正确处理没有结尾换行的最后一条 SSE；`length` 和 `content_filter` 不再被误判为成功。
- Markdown 嵌套列表增加可辨识的层级缩进，缩小项目符号并收紧项目间距。AI 与 IM 仍共用同一主题。

## 与 Codex 仍有差距

### 高优先级

- **上下文没有 token 预算、裁剪和压缩。** 每次请求都会重新组装并发送完整历史，长会话会越来越慢、越来越贵，最终由供应商拒绝。`MaiContextBuilder` 需要增加 token 估算、保留最近轮次和旧历史摘要。
- **Shell 没有操作系统沙箱。** 审批可以挡住已识别的高风险命令，但批准后命令拥有 MaiChat 进程本身的权限。Codex 的 workspace/sandbox 边界不能只靠字符串白名单替代。
- **API Key 目前保存在当前用户的 QSettings 中。** 输入框不会明文显示，日志也不记录 Key，但磁盘存储仍是明文配置；正式分发前应接入 macOS Keychain 与 Windows Credential Manager。

### 中优先级

- 只实现了 Chat Completions，`Responses` 目前仍返回空客户端。
- 核心没有 usage/token 统计，界面只能显示字符估数。
- 同一轮的多个工具调用按顺序执行；可以只并行确定为只读且互不依赖的调用。
- 流式正文在一次模型请求结束或中断时落库；进程在流式中途被系统直接终止时，当前尚未提交的增量可能丢失。
- “帮我批准”现在依据工具类别与工作目录边界判断，没有 Codex 的独立风险审查模型。

## 验证口径

- MaiAgent：14/14。
- MaiChat Desktop：46/46。
- GLM 编程套餐端点已用真实请求验证过 Chat Completions 流式连接；API Key 没有写入源码、测试数据或发布资产。
