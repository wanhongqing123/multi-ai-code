# Responses 线协议与 DeepSeek 主模型

`MaiModelClient` 是中立接口，调用方通过 `MaiModelConfig::wire` 选择
`ChatCompletions` 或 `Responses`。两者都接收完整的 `MaiModelRequest`，
模型、历史、工具和图片的线格式转换只在客户端边界发生。

DeepSeek V4.1 Flash 的 API 模型 ID 是 `deepseek-flash`，官方地址是
`https://api.deepseek.com`，Responses 请求发到 `/responses`。DeepSeek 的该接口
**无状态**；不能依赖 `previous_response_id`，每轮都须在 `input` 带完整历史。
资料：[V4.1 Flash 公告](https://api-docs.deepseek.com/zh-cn/news/news260910/)、
[Responses API](https://api-docs.deepseek.com/zh-cn/api/create-response/)、
[Responses 指南](https://api-docs.deepseek.com/zh-cn/guides/responses_api/)。

| 中立表示 | Responses 请求或事件 |
|---|---|
| 基础指令 | 顶层 `instructions` |
| 用户、系统、助手消息 | `input[]` 的 `message` item |
| 用户图片 | `input_image` 内容块；本地图片按原有 2 MB 请求预算准备 data URL |
| 助手工具调用 | `function_call` item，保留 `call_id`、名称及完整参数 |
| 工具结果 | `function_call_output` item，用同一个 `call_id` 配对 |
| 已保存的思考内容 | 独立 `reasoning` item；Chat 路径不把它混入正文 |
| 工具清单 | 顶层扁平的 `tools[]`，每项含 `type=function`、名称、描述和参数 Schema |
| 增量正文、思考、参数 | `response.output_text.delta`、`response.reasoning_text.delta`、`response.function_call_arguments.delta` |
| 请求结束 | 仅 `response.completed` 为成功；`response.incomplete` 和 `response.failed` 返回错误 |

流式事件可能按任意字节边界拆分。客户端按行重组 SSE，只向上层追加增量；
工具参数等完整后才发起工具调用。结束事件没有 Chat 的 `[DONE]`，
连接提前关闭不能当成功。网络重试只发生在尚未交付模型内容之前。

iOS 的 DeepSeek Key 存在独立 Keychain 项；桌面与 Android 也使用独立于 GLM 的
配置项。切换模型不复用另一服务商的 Key。用户在应用设置中填写一次 Key 后，
模型菜单可在 `glm-5.3`、`glm-5.3-flash` 和 `deepseek-flash` 之间切换。

本地模拟服务覆盖：SSE 任意分片、正文/思考去重、工具参数增量与完整结果、
完整历史及图片序列化、缺失结束事件和不完整响应。使用用户提供的 Key 做了
两次受控实时验收：普通 Responses 回复 `OK`，以及“模型调用 echo → 回传结果 →
模型继续回复 `OK`”。Key 未写入仓库或测试日志。
