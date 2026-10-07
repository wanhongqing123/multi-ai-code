// LLM 客户端的测试。
//
// 不打真实的大模型：一是要 API key、二是要钱、三是结果不确定。
// 这里起一个假的 Chat Completions 服务端，
// 喂**对抗性分片**的 SSE ——把工具调用的 arguments 一个字符一个字符地切、切在 JSON 的引号中间、
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
#include "MaiOpenAiClient.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

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
    int status = 200;
    int transientFailures = 0;
    bool stallWithoutBytes = false;
    bool sendOnlyHeartbeats = false;
    bool keepOpenAfterDone = false;
    std::vector<std::string> delayedChunks;
    int delayedChunkMs = 0;
    std::atomic<bool> releaseStall{false};
    std::atomic<int> requestCount{0};

    // 收到的请求体和鉴权头。测线格式要看**真正发出去的字节**，
    // 不能拿我们自己的结构体去对——那只能证明"符合我的理解"。
    std::mutex mutex;
    std::string lastBody;
    std::string lastAuthorization;

    void start() {
        auto handle = [this](const httplib::Request& request, httplib::Response& response) {
            const int requestNumber = ++requestCount;
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastBody = request.body;
                lastAuthorization = request.get_header_value("Authorization");
            }
            if (requestNumber <= transientFailures) {
                response.status = 503;
                response.set_content(R"({"error":{"message":"temporarily unavailable"}})",
                                     "application/json");
                return;
            }
            if (status != 200) {
                response.status = status;
                response.set_content(script, "application/json");
                return;
            }
            if (keepOpenAfterDone) {
                auto sent = std::make_shared<bool>(false);
                response.set_chunked_content_provider(
                    "text/event-stream", [this, sent](std::size_t, httplib::DataSink& sink) {
                        if (!*sent) {
                            *sent = true;
                            return sink.write(script.data(), script.size());
                        }
                        if (releaseStall.load()) {
                            sink.done();
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        return true;
                    });
                return;
            }
            if (!delayedChunks.empty()) {
                auto index = std::make_shared<std::size_t>(0);
                response.set_chunked_content_provider(
                    "text/event-stream", [this, index](std::size_t, httplib::DataSink& sink) {
                        if (*index >= delayedChunks.size()) {
                            sink.done();
                            return false;
                        }
                        const std::string& chunk = delayedChunks[(*index)++];
                        const bool written = sink.write(chunk.data(), chunk.size());
                        std::this_thread::sleep_for(std::chrono::milliseconds(delayedChunkMs));
                        return written;
                    });
                return;
            }
            if (stallWithoutBytes || sendOnlyHeartbeats) {
                response.set_chunked_content_provider(
                    "text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
                        if (releaseStall.load()) {
                            sink.done();
                            return false;
                        }
                        if (sendOnlyHeartbeats && !sink.write(": keepalive\n\n", 13)) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        return true;
                    });
                return;
            }
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
        };
        server.Post("/chat/completions", handle);
        server.Post("/responses", handle);
        port = server.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeServer() {
        releaseStall.store(true);
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

Collected runAgainst(FakeServer& fake, const MaiModelRequest& request,
                     MaiWireApi wire = MaiWireApi::ChatCompletions) {
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.apiKey = "test-key";
    config.wire = wire;
    auto client = makeMaiModelClient(config);

    Collected collected;
    MaiStreamSink sink;
    sink.onText = [&collected](std::string_view text) { collected.text.append(text); };
    sink.onReasoning = [&collected](std::string_view text) { collected.reasoning.append(text); };
    sink.onToolCall = [&collected](const MaiToolInvocation& invocation) {
        collected.calls.push_back(invocation);
    };

    const std::atomic<bool> cancel{false};
    const MaiError error = client->stream(request, sink, cancel);
    collected.error = error.message();
    collected.code = error.code();
    collected.done = !error;
    return collected;
}

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
    // 错误现在是返回值而不是回调：调用方不会漏接，而且能拿到错误码来区分"网断了"和"用户按了停"。
    const MaiError err = client->stream(request, h, cancel);
    c.error = err.message();
    c.code = err.code();
    c.done = !err;
    return c;
}

// ── 用 JSON 库构造 SSE 帧 ───────────────────────────────────────
// 不手写多层转义。arguments 本身是一段 JSON 文本，外面再套一层 JSON，
// 手数反斜杠几乎必错——第一版就多写了一层，测试红了，查了半天才发现是测试自己写错，
// 不是被测代码的问题。转义交给库最省事。
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
// 但这个用例**测的就是非 ASCII**——多字节字符被切在 chunk 边界上还能不能拼回来，
// 所以字节本身一个都不能改。
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

void test_http_error_preserves_provider_message() {
    FakeServer fake;
    fake.status = 429;
    fake.script = json{{"error", {{"message", "quota exhausted"}, {"code", "usage_limit"}}}}.dump();
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.3";
    const Collected result = runAgainst(fake, request);
    CHECK(result.code == MaiErrorCode::RateLimited);
    CHECK(result.error.find("HTTP 429") != std::string::npos);
    CHECK(result.error.find("quota exhausted") != std::string::npos);
    CHECK(result.error.find("usage_limit") != std::string::npos);
}

void test_finish_reason_length_is_not_reported_as_success() {
    json choice;
    choice["delta"] = json::object();
    choice["finish_reason"] = "length";
    const Collected result = run(sse(json{{"choices", json::array({choice})}}) + kDone, 3);
    CHECK(result.code == MaiErrorCode::Protocol);
    CHECK(result.error.find("长度上限") != std::string::npos);
}

void test_final_unterminated_sse_line_is_processed() {
    json root;
    root["error"] = "last line error";
    const std::string unterminated = "data: " + root.dump();
    const Collected result = run(unterminated, 2);
    CHECK(result.code == MaiErrorCode::Protocol);
    CHECK(result.error == "last line error");
}

void test_transient_http_failure_retries_before_streaming() {
    FakeServer fake;
    fake.transientFailures = 1;
    fake.script = text_delta("recovered") + kDone;
    fake.chunk = 4;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.3";
    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    CHECK(result.text == "recovered");
    CHECK(fake.requestCount.load() == 2);
}

void test_heartbeats_do_not_extend_model_inactivity() {
    FakeServer fake;
    fake.sendOnlyHeartbeats = true;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.inactivityTimeoutSeconds = 1;
    config.totalTimeoutSeconds = 4;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    const MaiError result = client->stream(request, sink, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(result.code() == MaiErrorCode::Network);
    CHECK(result.message().find("inactive") != std::string::npos);
    CHECK(result.message().find("0 images; received ") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds(3));
}

void test_real_model_progress_extends_idle_deadline() {
    FakeServer fake;
    fake.delayedChunks = {text_delta("first"), text_delta("second"), kDone};
    fake.delayedChunkMs = 1100;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.inactivityTimeoutSeconds = 2;
    config.totalTimeoutSeconds = 6;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    std::string output;
    MaiStreamSink sink;
    sink.onText = [&output](std::string_view text) { output.append(text); };
    const std::atomic<bool> cancel{false};
    const MaiError result = client->stream(request, sink, cancel);
    CHECK(!result);
    CHECK(output == "firstsecond");
}

void test_done_ends_stream_without_waiting_for_socket_close() {
    FakeServer fake;
    fake.keepOpenAfterDone = true;
    fake.script = text_delta("complete") + kDone;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.totalTimeoutSeconds = 3;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    std::string output;
    MaiStreamSink sink;
    sink.onText = [&output](std::string_view text) { output.append(text); };
    const std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    const MaiError result = client->stream(request, sink, cancel);
    CHECK(!result);
    CHECK(output == "complete");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void test_cancel_interrupts_silent_model_stream() {
    FakeServer fake;
    fake.stallWithoutBytes = true;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.totalTimeoutSeconds = 4;
    config.inactivityTimeoutSeconds = 10;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    std::atomic<bool> cancel{false};
    MaiError result;
    std::thread worker([&] { result = client->stream(request, sink, cancel); });
    for (int i = 0; i < 200 && fake.requestCount.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto canceledAt = std::chrono::steady_clock::now();
    cancel.store(true);
    worker.join();
    CHECK(result.code() == MaiErrorCode::Canceled);
    CHECK(std::chrono::steady_clock::now() - canceledAt < std::chrono::seconds(2));
}

// ── 线格式 ──────────────────────────────────────────────────────
//
// 这一组断言的是**真正发到 socket 上的 JSON**，对照 OpenAI Chat Completions 的规范，
// 不是对照我们自己的结构体。
//
// 这个区分是有代价才学到的：tool_call_id 曾经被写成了 toolCallId，
// 而当时的用例写的是 `m.value("toolCallId", "") == callId`——拿自己的字段名去核自己的输出，
// 永远是绿的。真跑起来服务端会说缺 tool_call_id，或者模型认不出这是哪次调用的结果，
// 下一轮把同样的工具再调一遍。
void test_wire_shape_of_request() {
    FakeServer fake;
    fake.script = std::string(": ping\n\n") + kDone;
    fake.chunk = 100000;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-4.6";
    request.baseInstructions = "Return valid Markdown.";

    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "read a.txt";
    request.messages.push_back(user);

    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.invocations.push_back(MaiToolInvocation{"call_1", "read", R"({"path":"a.txt"})"});
    request.messages.push_back(assistant);

    MaiModelMessage toolResult;
    toolResult.role = MaiModelRole::ToolResult;
    toolResult.toolCallId = "call_1";
    toolResult.content = "file body";
    request.messages.push_back(toolResult);

    MaiToolSpec spec;
    spec.name = "read";
    spec.description = "read a file";
    spec.parametersJson = R"({"type":"object","properties":{}})";
    request.tools.push_back(spec);

    runAgainst(fake, request);

    std::string body;
    std::string authorization;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
        authorization = fake.lastAuthorization;
    }
    CHECK(authorization == "Bearer test-key");

    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (sent.is_discarded()) return;

    CHECK(sent.value("model", "") == "glm-4.6");
    CHECK(sent.value("stream", false) == true);  // 不开流就收不到增量
    CHECK(sent["messages"].size() == 4);

    CHECK(sent["messages"][0]["role"] == "system");
    CHECK(sent["messages"][0]["content"] == request.baseInstructions);
    CHECK(sent["messages"][1]["role"] == "user");
    CHECK(sent["messages"][1]["content"] == "read a.txt");

    // assistant 发起调用那条：tool_calls 的嵌套必须对。
    const json& call = sent["messages"][2];
    CHECK(call["role"] == "assistant");
    CHECK(call["tool_calls"].size() == 1);
    if (call["tool_calls"].size() == 1) {
        CHECK(call["tool_calls"][0]["id"] == "call_1");
        CHECK(call["tool_calls"][0]["type"] == "function");
        CHECK(call["tool_calls"][0]["function"]["name"] == "read");
        // arguments 是 JSON **字符串**，不是对象。写成对象服务端会拒。
        CHECK(call["tool_calls"][0]["function"]["arguments"].is_string());
    }

    // 工具结果那条：字段名是 tool_call_id，snake_case。
    const json& result = sent["messages"][3];
    CHECK(result["role"] == "tool");
    CHECK(result.contains("tool_call_id"));
    CHECK(result.value("tool_call_id", "") == "call_1");
    CHECK(!result.contains("toolCallId"));  // 别再犯一次
    CHECK(result["content"] == "file body");

    // 工具清单的嵌套：type / function / {name, description, parameters}
    CHECK(sent["tools"].size() == 1);
    if (sent["tools"].size() == 1) {
        CHECK(sent["tools"][0]["type"] == "function");
        CHECK(sent["tools"][0]["function"]["name"] == "read");
        CHECK(sent["tools"][0]["function"]["parameters"].is_object());
    }
}

void test_no_tools_field_when_empty() {
    FakeServer fake;
    fake.script = std::string(": ping\n\n") + kDone;
    fake.chunk = 100000;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.3";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "chat";
    request.messages.push_back(user);

    runAgainst(fake, request);

    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    // 没工具就不带 tools 字段，而不是带一个空数组——有的服务端见到空数组会报错。
    CHECK(!sent.is_discarded() && !sent.contains("tools"));
}

void test_user_image_is_sent_as_multimodal_content() {
    FakeServer fake;
    fake.script = text_delta("ok") + std::string(kDone);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root =
        MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8("mai-model-image-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath image = root.append(MaiFilePath::fromUtf8("photo.png"));
    CHECK(!MaiFileSystem::writeFile(image, std::string("\x89PNG", 4)));

    MaiModelRequest request;
    request.model = "glm-5.3";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect";
    user.images.push_back({"photo.png", "image/png"});
    request.messages.push_back(user);

    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["messages"][0]["content"];
        CHECK(content.is_array());
        CHECK(content.size() == 2);
        if (content.size() == 2) {
            CHECK(content[0]["type"] == "text");
            CHECK(content[0]["text"] == "inspect");
            CHECK(content[1]["type"] == "image_url");
            CHECK(content[1]["image_url"]["url"] == "data:image/png;base64,iVBORw==");
        }
    }
    MaiFileSystem::removeRecursively(root);
}

void test_large_historical_image_uses_cached_bounded_preview() {
    FakeServer fake;
    fake.script = text_delta("ok") + std::string(kDone);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root =
        MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8("mai-model-preview-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath preview = root.append(MaiFilePath::fromUtf8("preview.jpg"));
    std::string large(3u * 1024u * 1024u, 'a');
    large.replace(0, 4, "\x89PNG", 4);
    std::vector<MaiFilePath> sources;
    for (int index = 0; index < 4; ++index) {
        sources.push_back(
            root.append(MaiFilePath::fromUtf8("historical-" + std::to_string(index) + ".png")));
        CHECK(!MaiFileSystem::writeFile(sources.back(), large));
    }
    CHECK(!MaiFileSystem::writeFile(preview, std::string("\xff\xd8\xff", 3)));

    int prepared = 0;
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.prepareImage = [&](const std::string& path,
                              const std::string& workspace) -> MaiResult<std::string> {
        ++prepared;
        bool matchesSource = false;
        for (const auto& source : sources)
            if (path == MaiFileSystem::resolve(source).toUtf8()) matchesSource = true;
        CHECK(matchesSource);
        CHECK(workspace == root.toUtf8());
        return preview.toUtf8();
    };
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "glm-5.3-flash";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect the prior frame";
    for (int index = 0; index < 4; ++index)
        user.images.push_back({"historical-" + std::to_string(index) + ".png", "image/png"});
    request.messages.push_back(user);
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    CHECK(!client->stream(request, sink, cancel));
    CHECK(!client->stream(request, sink, cancel));
    CHECK(prepared == 4);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    CHECK(body.size() < 3000);
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["messages"][0]["content"];
        CHECK(content.size() == 5);
        for (std::size_t index = 1; index < content.size(); ++index) {
            const std::string url = content[index]["image_url"]["url"];
            CHECK(url.rfind("data:image/jpeg;base64,", 0) == 0);
        }
    }
    MaiFileSystem::removeRecursively(root);
}

void test_absolute_desktop_image_can_be_outside_the_workspace() {
    FakeServer fake;
    fake.script = text_delta("ok") + std::string(kDone);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath temporary = MaiFileSystem::temporaryDirectory();
    const MaiFilePath workspace =
        temporary.append(MaiFilePath::fromUtf8("mai-model-absolute-image-workspace"));
    const MaiFilePath external =
        temporary.append(MaiFilePath::fromUtf8("mai-model-absolute-image-external"));
    MaiFileSystem::removeRecursively(workspace);
    MaiFileSystem::removeRecursively(external);
    CHECK(!MaiFileSystem::createDirectories(workspace));
    CHECK(!MaiFileSystem::createDirectories(external));
    const MaiFilePath image = external.append(MaiFilePath::fromUtf8("photo.png"));
    CHECK(!MaiFileSystem::writeFile(image, std::string("\x89PNG", 4)));

    MaiModelRequest request;
    request.model = "glm-5.3";
    request.workingDirectory = workspace.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect";
    user.images.push_back({image.toUtf8(), "image/png"});
    request.messages.push_back(user);

    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["messages"][0]["content"];
        CHECK(content.is_array());
        CHECK(content.size() == 2);
        if (content.size() == 2)
            CHECK(content[1]["image_url"]["url"] == "data:image/png;base64,iVBORw==");
    }
    MaiFileSystem::removeRecursively(workspace);
    MaiFileSystem::removeRecursively(external);
}

void test_missing_historical_image_does_not_break_later_turns() {
    FakeServer fake;
    fake.script = text_delta("ok") + std::string(kDone);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-model-missing-history-image-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));

    MaiModelRequest request;
    request.model = "glm-5.3";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage oldUser;
    oldUser.role = MaiModelRole::User;
    oldUser.content = "old image";
    oldUser.images.push_back({"removed.png", "image/png"});
    request.messages.push_back(oldUser);
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.content = "previous response";
    request.messages.push_back(assistant);
    MaiModelMessage currentUser;
    currentUser.role = MaiModelRole::User;
    currentUser.content = "continue";
    request.messages.push_back(currentUser);

    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["messages"][0]["content"];
        CHECK(content.is_array());
        CHECK(content.size() == 1);
        if (content.size() == 1) {
            CHECK(content[0]["type"] == "text");
            CHECK(content[0]["text"].get<std::string>().find("no longer available") !=
                  std::string::npos);
        }
        CHECK(sent["messages"].back()["content"] == "continue");
    }
    MaiFileSystem::removeRecursively(root);
}

void test_missing_current_image_is_reported() {
    FakeServer fake;
    fake.script = text_delta("unexpected") + std::string(kDone);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-model-missing-current-image-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));

    MaiModelRequest request;
    request.model = "glm-5.3";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect";
    user.images.push_back({"missing.png", "image/png"});
    request.messages.push_back(user);

    const Collected result = runAgainst(fake, request);
    CHECK(!result.done);
    CHECK(result.code != MaiErrorCode::Ok);
    CHECK(result.error.find("cannot open file for reading") != std::string::npos);
    CHECK(fake.requestCount.load() == 0);
    MaiFileSystem::removeRecursively(root);
}

void test_invalid_utf8_history_is_replaced_before_serialization() {
    FakeServer fake;
    fake.script = text_delta("ok") + std::string(kDone);
    fake.chunk = 100000;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.3";
    MaiModelMessage toolResult;
    toolResult.role = MaiModelRole::ToolResult;
    toolResult.toolCallId = "call_legacy";
    // GBK bytes for the beginning of a Windows shell error. Old databases may contain this
    // exact shape because redirected cmd.exe output was stored without conversion to UTF-8.
    toolResult.content.assign("\xCF\xB5\xCD\xB3\xD5\xD2", 6);
    request.messages.push_back(toolResult);

    bool threw = false;
    Collected result;
    try {
        result = runAgainst(fake, request);
    } catch (...) {
        threw = true;
    }

    CHECK(!threw);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    CHECK(sent["messages"].size() == 1);
    CHECK(sent["messages"][0]["role"] == "tool");
    CHECK(sent["messages"][0]["content"].get<std::string>().find("\xEF\xBF\xBD") !=
          std::string::npos);
}

void test_responses_streams_reasoning_text_and_function_calls() {
    FakeServer fake;
    fake.chunk = 1;
    fake.script =
        sse(json{{"type", "response.created"}}) +
        sse(json{{"type", "response.reasoning_text.delta"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"delta", "Think"}}) +
        sse(json{{"type", "response.output_text.delta"},
                 {"output_index", 1},
                 {"content_index", 0},
                 {"delta", "Ready"}}) +
        sse(json{
            {"type", "response.output_item.added"},
            {"output_index", 2},
            {"item", {{"type", "function_call"}, {"call_id", "call_7"}, {"name", "status"}}}}) +
        sse(json{{"type", "response.function_call_arguments.delta"},
                 {"output_index", 2},
                 {"delta", "{\"pa"}}) +
        sse(json{{"type", "response.function_call_arguments.delta"},
                 {"output_index", 2},
                 {"delta", "th\":\"a\"}"}}) +
        sse(json{{"type", "response.function_call_arguments.done"},
                 {"output_index", 2},
                 {"arguments", R"({"path":"a"})"}}) +
        sse(json{
            {"type", "response.completed"},
            {"response",
             {{"status", "completed"},
              {"output", json::array({json{{"type", "reasoning"},
                                           {"content", json::array({json{{"type", "reasoning_text"},
                                                                         {"text", "Think"}}})}},
                                      json{{"type", "message"},
                                           {"content", json::array({json{{"type", "output_text"},
                                                                         {"text", "Ready"}}})}},
                                      json{{"type", "function_call"},
                                           {"call_id", "call_7"},
                                           {"name", "status"},
                                           {"arguments", R"({"path":"a"})"}}})}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "deepseek-flash";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Check status";
    request.messages.push_back(std::move(user));
    const Collected result = runAgainst(fake, request, MaiWireApi::Responses);
    CHECK(result.done);
    CHECK(result.text == "Ready");
    CHECK(result.reasoning == "Think");
    CHECK(result.calls.size() == 1);
    if (result.calls.size() == 1) {
        CHECK(result.calls[0].id == "call_7");
        CHECK(result.calls[0].name == "status");
        CHECK(result.calls[0].arguments == R"({"path":"a"})");
    }
}

void test_responses_serializes_history_images_and_flat_tools() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-responses-image-test"));
    CHECK(!MaiFileSystem::createDirectories(root));
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("photo.png")),
                                    std::string("\x89PNG", 4)));
    MaiModelRequest request;
    request.model = "deepseek-flash";
    request.baseInstructions = "Follow the user request.";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Inspect this";
    user.images.push_back({"photo.png", "image/png"});
    request.messages.push_back(std::move(user));
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.reasoning = "I should check the value.";
    assistant.invocations.push_back({"call_8", "lookup", R"({"query":"x"})"});
    request.messages.push_back(std::move(assistant));
    MaiModelMessage result;
    result.role = MaiModelRole::ToolResult;
    result.toolCallId = "call_8";
    result.content = "found";
    request.messages.push_back(std::move(result));
    request.tools.push_back({"lookup", "Look up a value", R"({"type":"object"})"});
    const Collected answer = runAgainst(fake, request, MaiWireApi::Responses);
    CHECK(answer.done);
    std::string wire;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        wire = fake.lastBody;
    }
    const json body = json::parse(wire);
    CHECK(body["model"] == "deepseek-flash");
    CHECK(body["instructions"] == "Follow the user request.");
    CHECK(body["input"].size() == 4);
    CHECK(body["input"][0]["role"] == "user");
    CHECK(body["input"][0]["content"][1]["type"] == "input_image");
    CHECK(body["input"][0]["content"][1]["image_url"].get<std::string>().find(
              "data:image/png;base64,") == 0);
    CHECK(body["input"][1]["type"] == "reasoning");
    CHECK(body["input"][1]["content"][0]["text"] == "I should check the value.");
    CHECK(body["input"][2]["type"] == "function_call");
    CHECK(body["input"][2]["call_id"] == "call_8");
    CHECK(body["input"][3]["type"] == "function_call_output");
    CHECK(body["input"][3]["call_id"] == "call_8");
    CHECK(body["tools"][0]["name"] == "lookup");
    CHECK(!body["tools"][0].contains("function"));
    MaiFileSystem::removeRecursively(root);
}

void test_responses_needs_terminal_event_and_reports_incomplete() {
    MaiModelRequest request;
    request.model = "deepseek-flash";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Hello";
    request.messages.push_back(std::move(user));
    {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.output_text.delta"}, {"delta", "partial"}});
        fake.start();
        const Collected result = runAgainst(fake, request, MaiWireApi::Responses);
        CHECK(!result.done);
        CHECK(result.text == "partial");
        CHECK(result.error.find("without a terminal event") != std::string::npos);
    }
    {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.incomplete"},
                               {"response",
                                {{"output", json::array()},
                                 {"incomplete_details", {{"reason", "max_output_tokens"}}}}}});
        fake.start();
        const Collected result = runAgainst(fake, request, MaiWireApi::Responses);
        CHECK(!result.done);
        CHECK(result.error.find("max_output_tokens") != std::string::npos);
    }
    {
        FakeServer fake;
        fake.script = sse(
            json{{"type", "response.failed"},
                 {"response",
                  {{"output", json::array()},
                   {"error", {{"code", "provider_error"}, {"message", "generation failed"}}}}}});
        fake.start();
        const Collected result = runAgainst(fake, request, MaiWireApi::Responses);
        CHECK(!result.done);
        CHECK(result.error.find("generation failed") != std::string::npos);
    }
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
    test_http_error_preserves_provider_message();
    test_finish_reason_length_is_not_reported_as_success();
    test_final_unterminated_sse_line_is_processed();
    test_transient_http_failure_retries_before_streaming();
    test_heartbeats_do_not_extend_model_inactivity();
    test_real_model_progress_extends_idle_deadline();
    test_done_ends_stream_without_waiting_for_socket_close();
    test_cancel_interrupts_silent_model_stream();
    test_wire_shape_of_request();
    test_no_tools_field_when_empty();
    test_user_image_is_sent_as_multimodal_content();
    test_large_historical_image_uses_cached_bounded_preview();
    test_absolute_desktop_image_can_be_outside_the_workspace();
    test_missing_historical_image_does_not_break_later_turns();
    test_missing_current_image_is_reported();
    test_invalid_utf8_history_is_replaced_before_serialization();
    test_responses_streams_reasoning_text_and_function_calls();
    test_responses_serializes_history_images_and_flat_tools();
    test_responses_needs_terminal_event_and_reports_incomplete();
    if (failures == 0) std::printf("llm tests passed\n");
    return failures == 0 ? 0 : 1;
}
