// LLM 客户端的测试。
//
// 不打真实的大模型：一是要 API key、二是要钱、三是结果不确定。
// 这里起一个假的 Chat Completions 服务端，喂**对抗性分片**的 SSE ——
// 把工具调用的 arguments 一个字符一个字符地切、切在 JSON 的引号中间、
// 把多个事件塞进同一个 TCP 包、再掺几个心跳注释行和畸形行。
//
// 这正是真实服务端会干的事，也是这一层唯一真正难写的地方。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "mai/llm.h"

using namespace mai;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

namespace {

// 把一段完整的 SSE 文本按给定块大小切开发出去，模拟网络分片。
struct FakeServer {
  httplib::Server srv;
  std::thread th;
  int port = 0;
  std::string script;
  std::size_t chunk = 1;

  void start() {
    srv.Post("/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
      auto text = std::make_shared<std::string>(script);
      auto pos = std::make_shared<std::size_t>(0);
      const std::size_t step = chunk;
      res.set_chunked_content_provider(
          "text/event-stream",
          [text, pos, step](std::size_t, httplib::DataSink& sink) {
            if (*pos >= text->size()) {
              sink.done();
              return false;
            }
            const std::size_t n = std::min(step, text->size() - *pos);
            const bool ok = sink.write(text->data() + *pos, n);
            *pos += n;
            return ok;
          });
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    for (int i = 0; i < 200 && !srv.is_running(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ~FakeServer() {
    srv.stop();
    if (th.joinable()) th.join();
  }

  std::string base() const { return "http://127.0.0.1:" + std::to_string(port); }
};

struct Collected {
  std::string text;
  std::string reasoning;
  std::vector<ToolInvocation> calls;
  std::string error;
  ErrorCode code = ErrorCode::Ok;
  bool done = false;
};

Collected run(const std::string& script, std::size_t chunk) {
  FakeServer fake;
  fake.script = script;
  fake.chunk = chunk;
  fake.start();

  ModelConfig cfg;
  cfg.base_url = fake.base();
  cfg.api_key = "test-key";
  auto client = make_model_client(cfg);

  ModelRequest req;
  req.model = "glm-5.3";
  ModelMessage t;
  t.role = ModelRole::User;
  t.content = "跑一下测试";
  req.messages.push_back(t);

  Collected c;
  StreamSink h;
  h.on_text = [&c](std::string_view s) { c.text.append(s); };
  h.on_reasoning = [&c](std::string_view s) { c.reasoning.append(s); };
  h.on_tool_call = [&c](const ToolInvocation& t) { c.calls.push_back(t); };

  const std::atomic<bool> cancel{false};
  // 错误现在是返回值而不是回调：调用方不会漏接，而且能拿到错误码
  // 来区分"网断了"和"用户按了停"。
  const Error err = client->stream(req, h, cancel);
  c.error = err.message;
  c.code = err.code;
  c.done = !err;
  return c;
}

// ── 用 JSON 库构造 SSE 帧 ───────────────────────────────────────
// 不手写多层转义。arguments 本身是一段 JSON 文本，外面再套一层 JSON，
// 手数反斜杠几乎必错——第一版就多写了一层，测试红了，查了半天才发现
// 是测试自己写错，不是被测代码的问题。转义交给库最省事。
std::string sse(const json& j) { return "data: " + j.dump() + "\n\n"; }

// 构造一条 tool_calls 增量。传 nullptr 表示这一帧不带该字段。
std::string tool_delta(int index, const char* id, const char* name, const char* args) {
  json tc;
  tc["index"] = index;
  if (id) tc["id"] = id;
  json fn = json::object();
  if (name) fn["name"] = name;
  if (args) fn["arguments"] = args;
  tc["function"] = std::move(fn);

  json delta;
  delta["tool_calls"] = json::array({tc});
  json choice;
  choice["delta"] = std::move(delta);
  json root;
  root["choices"] = json::array({choice});
  return sse(root);
}

std::string text_delta(const char* content) {
  json delta;
  delta["content"] = content;
  json choice;
  choice["delta"] = std::move(delta);
  json root;
  root["choices"] = json::array({choice});
  return sse(root);
}

const char* kDone = "data: [DONE]\n\n";

std::string text_script() {
  return std::string(": ping\n\n") + text_delta("你好") + text_delta("，世界") +
         text_delta(" 🙂") + kDone;
}

// 工具调用：arguments 切成四段，切点故意落在 JSON 的引号和冒号中间。
// 四段拼起来应该正好是 {"command":"npm test"}
std::string tool_script() {
  json fin_delta = json::object();
  json fin_choice;
  fin_choice["delta"] = std::move(fin_delta);
  fin_choice["finish_reason"] = "tool_calls";
  json fin;
  fin["choices"] = json::array({fin_choice});

  return tool_delta(0, "call_abc", "bash", "") +
         tool_delta(0, nullptr, nullptr, "{\"comm") +
         tool_delta(0, nullptr, nullptr, "and\":\"npm ") +
         tool_delta(0, nullptr, nullptr, "test\"}") + sse(fin) + kDone;
}

// 两个并行调用，index 交错到达：先来 1 的参数，再来 0 的。
std::string two_tools_script() {
  json c0;
  c0["index"] = 0;
  c0["id"] = "c0";
  c0["function"] = json{{"name", "read"}, {"arguments", ""}};
  json c1;
  c1["index"] = 1;
  c1["id"] = "c1";
  c1["function"] = json{{"name", "grep"}, {"arguments", ""}};

  json delta;
  delta["tool_calls"] = json::array({c0, c1});
  json choice;
  choice["delta"] = std::move(delta);
  json root;
  root["choices"] = json::array({choice});

  return sse(root) + tool_delta(1, nullptr, nullptr, "{\"q\":1}") +
         tool_delta(0, nullptr, nullptr, "{\"p\":2}") + kDone;
}

// ── 用例 ────────────────────────────────────────────────────────

void test_text_stream_one_byte_at_a_time() {
  const auto c = run(text_script(), 1);  // 最恶劣的分片
  CHECK(c.error.empty());
  CHECK(c.done);
  CHECK(c.text == "你好，世界 🙂");
  CHECK(c.calls.empty());
}

void test_text_stream_all_at_once() {
  const auto c = run(text_script(), 100000);  // 另一个极端：全挤一个包
  CHECK(c.error.empty());
  CHECK(c.text == "你好，世界 🙂");
}

void test_tool_call_fragments() {
  for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{100000}}) {
    const auto c = run(tool_script(), chunk);
    CHECK(c.error.empty());
    CHECK(c.done);
    CHECK(c.calls.size() == 1);
    if (c.calls.size() == 1) {
      CHECK(c.calls[0].id == "call_abc");
      CHECK(c.calls[0].name == "bash");
      if (c.calls[0].arguments != "{\"command\":\"npm test\"}")
        std::printf("  chunk=%zu 实际 arguments = %s\n", chunk, c.calls[0].arguments.c_str());
      CHECK(c.calls[0].arguments == "{\"command\":\"npm test\"}");
      // 拼出来的必须是合法 JSON——这才是工具实现拿得到的东西
      CHECK(!json::parse(c.calls[0].arguments, nullptr, false).is_discarded());
    }
  }
}

void test_two_tool_calls_interleaved() {
  const auto c = run(two_tools_script(), 3);
  CHECK(c.error.empty());
  CHECK(c.calls.size() == 2);
  if (c.calls.size() == 2) {
    // 必须按 index 排序，不是按到达顺序
    CHECK(c.calls[0].id == "c0");
    CHECK(c.calls[0].name == "read");
    CHECK(c.calls[0].arguments == "{\"p\":2}");
    CHECK(c.calls[1].id == "c1");
    CHECK(c.calls[1].name == "grep");
    CHECK(c.calls[1].arguments == "{\"q\":1}");
  }
}

void test_malformed_lines_are_ignored() {
  const std::string script = std::string(": 心跳\n\n") +
                             "data: 这不是 JSON\n\n" +
                             "data: {\"choices\":[]}\n\n" +
                             text_delta("ok") +
                             "没有前缀的垃圾行\n\n" + kDone;
  const auto c = run(script, 5);
  CHECK(c.error.empty());  // 畸形行不该升级成错误
  CHECK(c.text == "ok");   // 正常那条仍要收到
  CHECK(c.done);
}

void test_server_error_inside_stream() {
  // 有的服务端不用 HTTP 状态码，把错误塞在正常的流里
  json root;
  root["error"] = "额度不足";
  const auto c = run(sse(root), 4);
  CHECK(c.error == "额度不足");
  CHECK(c.code == ErrorCode::Protocol);
  CHECK(!c.done);
}

void test_reasoning_delta() {
  json delta;
  delta["reasoning_content"] = "先想想";
  json choice;
  choice["delta"] = std::move(delta);
  json root;
  root["choices"] = json::array({choice});

  const auto c = run(sse(root) + text_delta("答案") + kDone, 2);
  CHECK(c.reasoning == "先想想");
  CHECK(c.text == "答案");
}

}  // namespace

int main() {
  test_text_stream_one_byte_at_a_time();
  test_text_stream_all_at_once();
  test_tool_call_fragments();
  test_two_tool_calls_interleaved();
  test_malformed_lines_are_ignored();
  test_server_error_inside_stream();
  test_reasoning_delta();
  if (failures == 0) std::printf("llm tests passed\n");
  return failures == 0 ? 0 : 1;
}
