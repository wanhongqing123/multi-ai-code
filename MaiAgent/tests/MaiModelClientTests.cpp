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

#include "MaiModelClient.h"

using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 把一段完整的 SSE 文本按给定块大小切开发出去，模拟网络分片。
struct FakeServer {
    httplib::Server server;
    std::thread th;
    int port = 0;
    std::string script;
    std::size_t chunk = 1;

    void start() {
        server.Post(
            "/chat/completions", [this](const httplib::Request&, httplib::Response& response) {
                auto text = std::make_shared<std::string>(script);
                auto pos = std::make_shared<std::size_t>(0);
                const std::size_t step = chunk;
                response.set_chunked_content_provider(
                    "text/event-stream", [text, pos, step](std::size_t, httplib::DataSink& sink) {
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
        port = server.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeServer() {
        server.stop();
        if (th.joinable()) th.join();
    }

    std::string base() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

struct Collected {
    std::string text;
    std::string reasoning;
    std::vector<MaiToolInvocation> calls;
    std::string error;
    MaiErrorCode code = MaiErrorCode::Ok;
    bool done = false;
};

Collected run(const std::string& script, std::size_t chunk) {
    FakeServer fake;
    fake.script = script;
    fake.chunk = chunk;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.apiKey = "test-key";
    auto client = makeMaiModelClient(config);

    MaiModelRequest request;
    request.model = "glm-5.3";
    MaiModelMessage t;
    t.role = MaiModelRole::User;
    t.content = "run the tests";
    request.messages.push_back(t);

    Collected c;
    MaiStreamSink h;
    h.onText = [&c](std::string_view s) { c.text.append(s); };
    h.onReasoning = [&c](std::string_view s) { c.reasoning.append(s); };
    h.onToolCall = [&c](const MaiToolInvocation& t) { c.calls.push_back(t); };

    const std::atomic<bool> cancel{false};
    // 错误现在是返回值而不是回调：调用方不会漏接，而且能拿到错误码
    // 来区分"网断了"和"用户按了停"。
    const MaiError err = client->stream(request, h, cancel);
    c.error = err.message();
    c.code = err.code();
    c.done = !err;
    return c;
}

// ── 用 JSON 库构造 SSE 帧 ───────────────────────────────────────
// 不手写多层转义。arguments 本身是一段 JSON 文本，外面再套一层 JSON，
// 手数反斜杠几乎必错——第一版就多写了一层，测试红了，查了半天才发现
// 是测试自己写错，不是被测代码的问题。转义交给库最省事。
std::string sse(const json& j) {
    return "data: " + j.dump() + "\n\n";
}

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

// 这三段合起来是 "你好，世界 🙂"。
//
// 写成 \u 转义而不是直接的汉字，是因为规范要求代码里除注释外不出现中文；
// 但这个用例**测的就是非 ASCII**——多字节字符被切在 chunk 边界上还能不能
// 拼回来，所以字节本身一个都不能改。
//
//   \u4f60\u597d       你好
//   \uff0c\u4e16\u754c  ，世界
//   \U0001F642        🙂
const char* kGreetingPart1 = "\u4f60\u597d";
const char* kGreetingPart2 = "\uff0c\u4e16\u754c";
const char* kGreetingPart3 = " \U0001F642";

std::string greeting() {
    return std::string(kGreetingPart1) + kGreetingPart2 + kGreetingPart3;
}

std::string text_script() {
    return std::string(": ping\n\n") + text_delta(kGreetingPart1) + text_delta(kGreetingPart2) +
           text_delta(kGreetingPart3) + kDone;
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

    return tool_delta(0, "call_abc", "bash", "") + tool_delta(0, nullptr, nullptr, "{\"comm") +
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
    CHECK(c.text == greeting());
    CHECK(c.calls.empty());
}

void test_text_stream_all_at_once() {
    const auto c = run(text_script(), 100000);  // 另一个极端：全挤一个包
    CHECK(c.error.empty());
    CHECK(c.text == greeting());
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
                std::printf("  chunk=%zu actual arguments = %s\n", chunk,
                            c.calls[0].arguments.c_str());
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
    const std::string script = std::string(": heartbeat\n\n") + "data: not json at all\n\n" +
                               "data: {\"choices\":[]}\n\n" + text_delta("ok") +
                               "garbage line with no prefix\n\n" + kDone;
    const auto c = run(script, 5);
    CHECK(c.error.empty());  // 畸形行不该升级成错误
    CHECK(c.text == "ok");   // 正常那条仍要收到
    CHECK(c.done);
}

void test_server_error_inside_stream() {
    // 有的服务端不用 HTTP 状态码，把错误塞在正常的流里
    json root;
    root["error"] = "quota exhausted";
    const auto c = run(sse(root), 4);
    CHECK(c.error == "quota exhausted");
    CHECK(c.code == MaiErrorCode::Protocol);
    CHECK(!c.done);
}

void test_reasoning_delta() {
    json delta;
    delta["reasoning_content"] = "let me think";
    json choice;
    choice["delta"] = std::move(delta);
    json root;
    root["choices"] = json::array({choice});

    const auto c = run(sse(root) + text_delta("the answer") + kDone, 2);
    CHECK(c.reasoning == "let me think");
    CHECK(c.text == "the answer");
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
