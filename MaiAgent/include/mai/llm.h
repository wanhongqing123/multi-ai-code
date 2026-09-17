#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mai/types.h"

namespace mai {

// ── 中立的模型抽象 ──────────────────────────────────────────────
// 这里的结构**不是** OpenAI 的线格式，是我们自己的中间表示。
// 各个 wire 实现（Chat Completions / Responses / Anthropic）负责把它
// 翻译成自家的请求、把自家的响应翻译回这里的回调。
//
// 之前这一层直接长成了 OpenAI 的形状（ChatMessage/tool_calls 原样照搬），
// 那样加第二个供应商时要么污染这个头，要么在上层写一堆 if。

enum class WireApi {
  ChatCompletions,  // 第一版只实现这个
  Responses,        // 枚举先立着，加实现时只动工厂函数
};
const char* to_string(WireApi w);

// 模型发起的一次工具调用。arguments 保持 JSON 原文——
// 核心不解析它，交给工具实现去解，这样加新工具不用动这一层。
struct ToolInvocation {
  std::string id;
  std::string name;
  std::string arguments;
};

// 发给模型的一条消息里，说话的是谁。
// 独立成枚举而不是嵌在 ModelMessage 里，是因为它会被单独传递
// （wire 实现要把它映射成各家的 role 字符串）。
enum class ModelRole {
  System,
  User,
  Assistant,
  ToolResult,  // 一次工具调用的结果，协议里是 role="tool"
};

// 发给模型的一条消息。刻意和领域模型的 Message 分开：
// Message 是"我们怎么存"，这个是"模型怎么看"，两者的演化节奏不一样。
struct ModelMessage {
  ModelRole role = ModelRole::User;
  std::string content;
  std::string tool_call_id;                 // ToolResult 时必填
  std::vector<ToolInvocation> invocations;  // Assistant 发起调用时
};

struct ToolSpec {
  std::string name;
  std::string description;
  std::string parameters_json;  // JSON Schema 原文
};

struct ModelRequest {
  std::string model;
  std::vector<ModelMessage> messages;
  std::vector<ToolSpec> tools;
  double temperature = -1.0;  // < 0 表示不传，用服务端默认
};

// ── 流式回调 ────────────────────────────────────────────────────
//
// 线程契约（重要）：这些回调**在网络读线程上同步调用**。
// 实现方不要在里面做慢活（写 socket、落盘、等锁），否则会拖慢模型读取。
// 需要慢处理就自己塞队列——HTTP 适配器的 SSE 就是这么做的。
struct StreamSink {
  std::function<void(std::string_view)> on_text;
  std::function<void(std::string_view)> on_reasoning;
  // 工具调用是**攒完整**才给的。流式过程中参数是碎片，中途给会是半截 JSON。
  std::function<void(const ToolInvocation&)> on_tool_call;
};

struct ModelConfig {
  std::string base_url;
  std::string api_key;
  WireApi wire = WireApi::ChatCompletions;
  long connect_timeout_sec = 20;
  long total_timeout_sec = 0;  // 0 = 不限；流式对话可能很久
};

// 模型客户端。一个接口，多个 wire 实现。
class ModelClient {
 public:
  virtual ~ModelClient() = default;

  // 阻塞直到这一轮结束。
  // cancel 置 true 会让传输尽快中断——这是 Interrupt 的落点，
  // 比等超时干净。主动取消返回的是 ErrorCode::Canceled，**不是故障**。
  virtual Error stream(const ModelRequest& req, const StreamSink& sink,
                       const std::atomic<bool>& cancel) = 0;

  virtual WireApi wire() const = 0;
};

std::unique_ptr<ModelClient> make_model_client(ModelConfig config);

}  // namespace mai
