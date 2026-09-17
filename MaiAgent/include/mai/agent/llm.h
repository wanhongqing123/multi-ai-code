#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 这里是"边界"之一：和模型供应商对接的地方。实现里会用 JSON。
// 但接口本身不出现任何 JSON 类型——领域模型永远是原生结构体。

namespace mai::agent {

// 线格式。第一版只实现 ChatCompletions。
//
// 枚举现在就立两个值，是从 codex 学的教训反过来做：它的 WireApi 只剩一个变体
// （chat 被移除了），想加回来得改一圈类型。先立着，加实现时只动一处。
//
// 两者不是变体，是两种协议，连 base URL 都不同。以智谱为例：
//   ChatCompletions  open.bigmodel.cn/api/paas/v4
//   Responses        open.bigmodel.cn/api/v1
enum class WireApi { ChatCompletions, Responses };

struct ToolCall {
  std::string id;         // tool_call_id，回灌结果时要原样带回
  std::string name;
  std::string arguments;  // JSON 原文，不在这里解析——交给工具实现
};

struct ChatMessage {
  std::string role;          // system / user / assistant / tool
  std::string content;
  std::string tool_call_id;  // role == "tool" 时必填
  std::vector<ToolCall> tool_calls;  // role == "assistant" 且发起了调用时
};

struct ToolDef {
  std::string name;
  std::string description;
  std::string parameters_json;  // JSON Schema 原文
};

struct ChatRequest {
  std::string model;
  std::vector<ChatMessage> messages;
  std::vector<ToolDef> tools;
  double temperature = -1.0;  // < 0 表示不传，用服务端默认
};

// 流式回调。全部在 curl 的读线程上调用，实现方要自己保证线程安全。
struct StreamHandler {
  std::function<void(std::string_view)> on_text;       // 文本增量
  std::function<void(std::string_view)> on_reasoning;  // 推理增量（有的模型有）
  std::function<void(const ToolCall&)> on_tool_call;   // 工具调用，**已拼完整**
  std::function<void(const std::string&)> on_error;
  std::function<void()> on_done;
};

struct LlmConfig {
  std::string base_url;  // 例：https://open.bigmodel.cn/api/paas/v4
  std::string api_key;
  WireApi wire = WireApi::ChatCompletions;
  long connect_timeout_sec = 20;
  long total_timeout_sec = 0;  // 0 = 不限；流式对话可能很久，默认别设
};

class LlmClient {
 public:
  virtual ~LlmClient() = default;

  // 阻塞直到这一轮结束或被取消。返回 false 表示出错（细节已经走 on_error 给过）。
  // cancel 置 true 会让传输尽快中断——这是 Interrupt 的落点。
  virtual bool stream(const ChatRequest& req, const StreamHandler& handler,
                      const std::atomic<bool>& cancel) = 0;
};

std::unique_ptr<LlmClient> make_openai_client(LlmConfig config);

}  // namespace mai::agent
