#include "mai/llm.h"

#include <curl/curl.h>
#include <json.hpp>

#include <cstring>
#include <map>

namespace mai {
namespace {

using json = nlohmann::json;

// ── 按行切分流式字节 ────────────────────────────────────────────
// curl 的写回调给的是任意大小的字节块，一个 SSE 事件可能被劈成几块，
// 也可能一块里塞了好几个事件。所以必须自己缓冲按 \n 切。
//
// scanned_ 记住"已扫描过、确认不含换行"的前缀长度，避免每来一块就把
// 整个缓冲区重扫一遍——流式期间这个回调每秒被调几十次。
// 思路取自 codex 的 ollama/src/line_buffer.rs（那边 32 行）。
class LineBuffer {
 public:
  void append(const char* data, std::size_t len) { buf_.append(data, len); }

  bool next_line(std::string& out) {
    const std::size_t pos = buf_.find('\n', scanned_);
    if (pos == std::string::npos) {
      scanned_ = buf_.size();
      return false;
    }
    out.assign(buf_, 0, pos);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    buf_.erase(0, pos + 1);
    scanned_ = 0;
    return true;
  }

 private:
  std::string buf_;
  std::size_t scanned_ = 0;
};

// ── 工具调用的分片聚合 ──────────────────────────────────────────
// 这是 Chat Completions 最容易写错的一处。arguments 不是一次给全的，
// 是 JSON 字符串的碎片，按 index 分批到达：
//   {index:0, id:"call_x", function:{name:"bash", arguments:""}}
//   {index:0,             function:{arguments:"{\"comm"}}
//   {index:0,             function:{arguments:"and\":\"npm"}}
//   {index:0,             function:{arguments:" test\"}"}}
// 不同服务端分片时机不一样——有的整块给，有的一个字符一个字符给，
// 两种都要能处理，所以只能按 index 攒，等流结束再交付。
class InvocationAccumulator {
 public:
  void feed(const json& delta_tool_calls) {
    if (!delta_tool_calls.is_array()) return;
    for (const auto& tc : delta_tool_calls) {
      // index 缺省当 0：个别服务端在只有一个调用时会省掉它。
      const int idx = tc.value("index", 0);
      auto& slot = slots_[idx];
      if (tc.contains("id") && tc["id"].is_string()) slot.id = tc["id"].get<std::string>();
      if (!tc.contains("function")) continue;
      const auto& fn = tc["function"];
      if (fn.contains("name") && fn["name"].is_string()) {
        // name 也可能分片，所以是 append 不是赋值。
        slot.name += fn["name"].get<std::string>();
      }
      if (fn.contains("arguments") && fn["arguments"].is_string()) {
        slot.arguments += fn["arguments"].get<std::string>();
      }
    }
  }

  std::vector<ToolInvocation> take() {
    std::vector<ToolInvocation> out;
    out.reserve(slots_.size());
    for (auto& [_, c] : slots_) {  // map 保证按 index 有序
      if (c.name.empty()) continue;
      out.push_back(std::move(c));
    }
    slots_.clear();
    return out;
  }

  bool empty() const { return slots_.empty(); }

 private:
  std::map<int, ToolInvocation> slots_;
};

// 中立的 Speaker -> OpenAI 的 role 字符串。
// 这个映射是**这一层的职责**：上层用自己的词汇，翻译只发生在边界。
const char* role_of(Turn::Speaker s) {
  switch (s) {
    case Turn::Speaker::System:     return "system";
    case Turn::Speaker::User:       return "user";
    case Turn::Speaker::Assistant:  return "assistant";
    case Turn::Speaker::ToolResult: return "tool";
  }
  return "user";
}

// ── 请求体构造：中立结构 -> OpenAI 线格式 ─────────────────────
std::string build_body(const Completion& req) {
  json msgs = json::array();
  for (const auto& m : req.turns) {
    json jm{{"role", role_of(m.speaker)}};
    // assistant 发起调用的那条，content 可以是 null，但必须带 tool_calls。
    if (!m.content.empty() || m.invocations.empty()) jm["content"] = m.content;
    if (!m.tool_call_id.empty()) jm["tool_call_id"] = m.tool_call_id;
    if (!m.invocations.empty()) {
      json calls = json::array();
      for (const auto& c : m.invocations) {
        calls.push_back({{"id", c.id},
                         {"type", "function"},
                         {"function", {{"name", c.name}, {"arguments", c.arguments}}}});
      }
      jm["tool_calls"] = std::move(calls);
    }
    msgs.push_back(std::move(jm));
  }

  json body{{"model", req.model}, {"messages", std::move(msgs)}, {"stream", true}};
  if (req.temperature >= 0.0) body["temperature"] = req.temperature;

  if (!req.tools.empty()) {
    json tools = json::array();
    for (const auto& t : req.tools) {
      json params = json::parse(t.parameters_json, nullptr, /*allow_exceptions=*/false);
      if (params.is_discarded()) params = json::object();
      tools.push_back({{"type", "function"},
                       {"function",
                        {{"name", t.name},
                         {"description", t.description},
                         {"parameters", std::move(params)}}}});
    }
    body["tools"] = std::move(tools);
  }
  return body.dump();
}

// ── curl 回调的上下文 ───────────────────────────────────────────
struct StreamCtx {
  const StreamSink* sink;
  const std::atomic<bool>* cancel;
  LineBuffer lines;
  InvocationAccumulator tools;
  std::string error;
  bool done = false;
};

void handle_sse_line(StreamCtx& ctx, const std::string& line) {
  if (line.empty()) return;
  if (line[0] == ':') return;                    // 注释 / 心跳
  if (line.rfind("data:", 0) != 0) return;       // 只关心 data 行

  std::string payload = line.substr(5);
  if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
  if (payload == "[DONE]") {
    ctx.done = true;
    return;
  }

  // 服务端可能推来半截或畸形 JSON，不能让它把整轮搞崩。
  const json j = json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return;

  // 有的服务端把错误塞在正常流里而不是用 HTTP 状态码。
  if (j.contains("error")) {
    ctx.error = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump();
    return;
  }

  if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
  const auto& choice = j["choices"][0];
  if (!choice.contains("delta")) return;
  const auto& delta = choice["delta"];

  if (delta.contains("content") && delta["content"].is_string()) {
    const auto s = delta["content"].get<std::string>();
    if (!s.empty() && ctx.sink->on_text) ctx.sink->on_text(s);
  }
  // 推理增量各家字段名不统一，这两个是见得最多的。
  for (const char* key : {"reasoning_content", "reasoning"}) {
    if (delta.contains(key) && delta[key].is_string()) {
      const auto s = delta[key].get<std::string>();
      if (!s.empty() && ctx.sink->on_reasoning) ctx.sink->on_reasoning(s);
    }
  }
  if (delta.contains("tool_calls")) ctx.tools.feed(delta["tool_calls"]);
}

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto& ctx = *static_cast<StreamCtx*>(userdata);
  const std::size_t total = size * nmemb;
  // 返回不等于 total 的值会让 curl 以 CURLE_WRITE_ERROR 中断传输——
  // 这就是 Interrupt 的落点，比等超时干净。
  if (ctx.cancel->load(std::memory_order_relaxed)) return 0;

  ctx.lines.append(ptr, total);
  std::string line;
  while (ctx.lines.next_line(line)) handle_sse_line(ctx, line);
  return total;
}

// Chat Completions 的实现。Responses 将来是同一个接口的另一个实现，
// 上层一行都不用改——这正是把 ModelClient 做成中立抽象的目的。
class ChatCompletionsClient final : public ModelClient {
 public:
  explicit ChatCompletionsClient(ModelConfig cfg) : cfg_(std::move(cfg)) {}

  WireApi wire() const override { return WireApi::ChatCompletions; }

  Error stream(const Completion& req, const StreamSink& sink,
               const std::atomic<bool>& cancel) override {

    CURL* curl = curl_easy_init();
    if (!curl) return Error::make(ErrorCode::Internal, "curl_easy_init 失败");

    std::string url = cfg_.base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    const std::string body = build_body(req);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    // 我们自己按行解析 SSE，不要中间层做任何缓冲/合并。
    headers = curl_slist_append(headers, "Cache-Control: no-cache");
    std::string auth;
    if (!cfg_.api_key.empty()) {
      auth = "Authorization: Bearer " + cfg_.api_key;
      headers = curl_slist_append(headers, auth.c_str());
    }

    StreamCtx ctx{&sink, &cancel, {}, {}, {}, false};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_sec);
    if (cfg_.total_timeout_sec > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg_.total_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // 多线程下必须，否则 alarm 会乱
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // 允许 gzip，省流量
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MaiAgent/0.1");

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // 主动取消**不是故障**：单独一个错误码，让上层能区分"用户按了停"
    // 和"网断了"，界面才知道该不该弹错误。
    if (cancel.load(std::memory_order_relaxed))
      return Error::make(ErrorCode::Canceled, "已取消");

    if (rc != CURLE_OK)
      return Error::make(ErrorCode::Network,
                         std::string("curl: ") + curl_easy_strerror(rc));
    if (status >= 400)
      return Error::make(status == 401 || status == 403 ? ErrorCode::NotConfigured
                                                        : ErrorCode::Protocol,
                         "HTTP " + std::to_string(status));
    if (!ctx.error.empty()) return Error::make(ErrorCode::Protocol, ctx.error);

    // 工具调用攒到流结束才交付——中途交付会拿到半截 JSON。
    if (sink.on_tool_call) {
      for (const auto& c : ctx.tools.take()) sink.on_tool_call(c);
    }
    return Error::ok();
  }

 private:
  ModelConfig cfg_;
};

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

}  // namespace

std::unique_ptr<ModelClient> make_model_client(ModelConfig config) {
  // curl_global_init 不是线程安全的，用函数内静态保证只跑一次。
  static CurlGlobal once;
  (void)once;
  switch (config.wire) {
    case WireApi::ChatCompletions:
      return std::make_unique<ChatCompletionsClient>(std::move(config));
    case WireApi::Responses:
      // 枚举已经立着，加实现时只动这里。
      return nullptr;
  }
  return nullptr;
}

const char* to_string(WireApi w) {
  switch (w) {
    case WireApi::ChatCompletions: return "chat_completions";
    case WireApi::Responses:       return "responses";
  }
  return "chat_completions";
}

}  // namespace mai
