// agent loop 的测试：从 submit(Prompt) 到事件流吐完的完整一圈。
//
// 用假的 Chat Completions 服务端当模型，所以不需要 API key、不花钱、
// 结果完全确定。真实 GLM 说的是同一个协议，切过去只改 baseUrl。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiAgent.h"

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

// ── 假模型：一个真的 Chat Completions 端点 ─────────────────────
struct FakeModel {
    httplib::Server srv;
    std::thread th;
    int port = 0;

    // 每个 content 片段之间的停顿，用来模拟"慢慢吐字"，好测中断。
    int delayMilliseconds = 0;
    std::vector<std::string> chunks{"你好", "，我是", "测试模型"};
    std::string reasoning;

    // 收到的最后一个请求体，用来断言我们发出去的东西对不对。
    std::mutex mu;
    std::string lastBody;

    void start() {
        srv.Post("/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lock(mu);
                lastBody = req.body;
            }
            auto idx = std::make_shared<std::size_t>(0);
            auto sentReasoning = std::make_shared<bool>(reasoning.empty());
            const auto chunkList = chunks;
            const auto reasoningText = reasoning;
            const int delay = delayMilliseconds;

            res.set_chunked_content_provider(
                "text/event-stream", [idx, sentReasoning, chunkList, reasoningText, delay](
                                         std::size_t, httplib::DataSink& sink) {
                    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

                    if (!*sentReasoning) {
                        *sentReasoning = true;
                        json d;
                        d["reasoning_content"] = reasoningText;
                        json c;
                        c["delta"] = std::move(d);
                        json root;
                        root["choices"] = json::array({c});
                        const std::string f = "data: " + root.dump() + "\n\n";
                        return sink.write(f.data(), f.size());
                    }
                    if (*idx < chunkList.size()) {
                        json d;
                        d["content"] = chunkList[*idx];
                        json c;
                        c["delta"] = std::move(d);
                        json root;
                        root["choices"] = json::array({c});
                        ++*idx;
                        const std::string f = "data: " + root.dump() + "\n\n";
                        return sink.write(f.data(), f.size());
                    }
                    const std::string done = "data: [DONE]\n\n";
                    const bool ok = sink.write(done.data(), done.size());
                    sink.done();
                    return ok;
                });
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        for (int i = 0; i < 200 && !srv.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeModel() {
        srv.stop();
        if (th.joinable()) th.join();
    }

    std::string base() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
    std::string body() {
        std::lock_guard<std::mutex> lock(mu);
        return lastBody;
    }
};

// 把事件流录下来，方便断言顺序和内容。
struct Recorder {
    std::mutex mu;
    std::vector<MaiEvent> events;

    void attach(MaiAgent& a) {
        a.eventBus().subscribe([this](const MaiEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
        });
    }

    std::vector<MaiEvent> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return events;
    }

    std::size_t count(MaiEventType t) {
        std::lock_guard<std::mutex> lock(mu);
        std::size_t n = 0;
        for (const auto& e : events)
            if (e.type == t) ++n;
        return n;
    }

    // 把某个 part 上的所有 delta 按顺序拼起来——
    // 这正是界面做的事，拼出来应该等于完整正文。
    std::string assemble(const std::string& field_owner_part) {
        std::lock_guard<std::mutex> lock(mu);
        std::string out;
        for (const auto& e : events)
            if (e.type == MaiEventType::MessagePartDelta && e.partId == field_owner_part)
                out += e.delta;
        return out;
    }

    std::string firstDeltaPartId() {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& e : events)
            if (e.type == MaiEventType::MessagePartDelta) return e.partId;
        return {};
    }
};

std::unique_ptr<MaiAgent> make_agent(const FakeModel& model) {
    MaiModelConfig cfg;
    cfg.baseUrl = model.base();
    cfg.apiKey = "test";
    MaiAgent::Options opts;
    opts.defaultModel = "glm-5.3";
    return std::make_unique<MaiAgent>(makeMaiMemoryStore(), makeMaiModelClient(cfg), nullptr, opts);
}

// ── 用例 ────────────────────────────────────────────────────────

void test_full_turn() {
    FakeModel model;
    model.start();
    auto agent = make_agent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    const std::string msg_id = agent->submit(MaiSendPrompt{sid, "你是谁？"}).value();
    CHECK(!msg_id.empty());
    CHECK(msg_id.rfind("msg_", 0) == 0);

    agent->waitIdle();

    // 1. 事件顺序：至少要有 delta，最后要有 idle
    CHECK(rec.count(MaiEventType::MessagePartDelta) == 3);  // 三个 chunk
    CHECK(rec.count(MaiEventType::SessionIdle) == 1);
    CHECK(rec.count(MaiEventType::SessionError) == 0);

    // 2. part id 全程稳定 —— 换 id 会让界面重绘甚至闪屏
    const auto all = rec.snapshot();
    std::string partId;
    for (const auto& e : all) {
        if (e.type != MaiEventType::MessagePartDelta) continue;
        if (partId.empty()) partId = e.partId;
        CHECK(e.partId == partId);
        CHECK(e.messageId == msg_id);
        CHECK(e.field == "text");
    }
    CHECK(partId.rfind("prt_", 0) == 0);

    // 3. 界面把 delta 拼起来 == 完整正文
    CHECK(rec.assemble(partId) == "你好，我是测试模型");

    // 4. 落库的内容一致，刷新后不会变样
    const auto msgs = agent->listMessages(sid);
    CHECK(msgs.size() == 2);  // user + assistant
    if (msgs.size() == 2) {
        CHECK(msgs[0].role == MaiRole::User);
        CHECK(msgs[1].role == MaiRole::Assistant);
        CHECK(msgs[1].completed > 0);
        CHECK(msgs[1].parts.size() == 1);
        if (msgs[1].parts.size() == 1) {
            CHECK(msgs[1].parts[0].id == partId);  // 落库的 id 和事件里的一致
            const auto* t = std::get_if<MaiTextPart>(&msgs[1].parts[0].body);
            CHECK(t != nullptr);
            if (t) CHECK(t->text == "你好，我是测试模型");
        }
    }

    // 5. 标题自动从第一句话来，不再是"新会话"
    MaiSession s;
    CHECK(agent->getSession(sid, s));
    CHECK(s.title == "你是谁？");
}

void test_request_body_is_correct() {
    FakeModel model;
    model.start();
    auto agent = make_agent(model);

    const std::string sid = agent->submit(MaiCreateSession{"/tmp", "", "glm-4.6"}).value();
    agent->submit(MaiSendPrompt{sid, "第一句"});
    agent->waitIdle();

    const json b = json::parse(model.body(), nullptr, false);
    CHECK(!b.is_discarded());
    CHECK(b.value("model", "") == "glm-4.6");  // 会话上的 model 覆盖默认值
    CHECK(b.value("stream", false) == true);
    CHECK(b["messages"].size() == 1);
    CHECK(b["messages"][0]["role"] == "user");
    CHECK(b["messages"][0]["content"] == "第一句");
}

void test_multi_turn_history() {
    FakeModel model;
    model.start();
    auto agent = make_agent(model);

    const std::string sid = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "第一句"});
    agent->waitIdle();
    agent->submit(MaiSendPrompt{sid, "第二句"});
    agent->waitIdle();

    // 第二轮必须带上完整历史，否则模型没有上下文
    const json b = json::parse(model.body(), nullptr, false);
    CHECK(b["messages"].size() == 3);  // user + assistant + user
    if (b["messages"].size() == 3) {
        CHECK(b["messages"][0]["content"] == "第一句");
        CHECK(b["messages"][1]["role"] == "assistant");
        CHECK(b["messages"][1]["content"] == "你好，我是测试模型");
        CHECK(b["messages"][2]["content"] == "第二句");
    }
    CHECK(agent->listMessages(sid).size() == 4);
}

void test_reasoning_goes_to_its_own_part() {
    FakeModel model;
    model.reasoning = "让我想想";
    model.start();
    auto agent = make_agent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "算一下"});
    agent->waitIdle();

    // reasoning 和 text 必须是两个不同的 part，界面才能分开显示
    const auto all = rec.snapshot();
    std::vector<std::string> part_ids;
    for (const auto& e : all) {
        if (e.type != MaiEventType::MessagePartDelta) continue;
        if (std::find(part_ids.begin(), part_ids.end(), e.partId) == part_ids.end())
            part_ids.push_back(e.partId);
    }
    CHECK(part_ids.size() == 2);

    const auto msgs = agent->listMessages(sid);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) {
        CHECK(msgs[1].parts.size() == 2);
        if (msgs[1].parts.size() == 2) {
            CHECK(std::get_if<MaiReasoningPart>(&msgs[1].parts[0].body) != nullptr);
            CHECK(std::get_if<MaiTextPart>(&msgs[1].parts[1].body) != nullptr);
        }
    }

    // reasoning 不该回灌给模型——它是草稿，会污染下一轮上下文
    agent->submit(MaiSendPrompt{sid, "继续"});
    agent->waitIdle();
    const json b = json::parse(model.body(), nullptr, false);
    bool leaked = false;
    for (const auto& m : b["messages"])
        if (m.value("content", std::string{}).find("让我想想") != std::string::npos) leaked = true;
    CHECK(!leaked);
}

void test_interrupt() {
    FakeModel model;
    model.delayMilliseconds = 150;  // 慢慢吐，好让我们插进去
    model.chunks = {"一", "二", "三", "四", "五", "六", "七", "八"};
    model.start();
    auto agent = make_agent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "数数"});

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(agent->isBusy(sid));
    CHECK(agent->submit(MaiInterrupt{sid}).isOk());

    agent->waitIdle();
    CHECK(!agent->isBusy(sid));

    // 中断不算错误，而且已经吐出来的内容要保住
    CHECK(rec.count(MaiEventType::SessionError) == 0);
    CHECK(rec.count(MaiEventType::SessionIdle) == 1);
    const std::size_t got = rec.count(MaiEventType::MessagePartDelta);
    CHECK(got > 0);
    CHECK(got < 8);  // 确实提前停了
    const auto msgs = agent->listMessages(sid);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) CHECK(!msgs[1].parts.empty());  // 半截内容也落库
}

void test_busy_session_rejects_second_turn() {
    FakeModel model;
    model.delayMilliseconds = 120;
    model.start();
    auto agent = make_agent(model);

    const std::string sid = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    CHECK(agent->submit(MaiSendPrompt{sid, "第一句"}).isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // 不排队而是拒绝：排队会让用户以为消息丢了，界面上看不出区别
    CHECK(!agent->submit(MaiSendPrompt{sid, "第二句"}).isOk());
    agent->waitIdle();
    CHECK(agent->listMessages(sid).size() == 2);
}

void test_unknown_session() {
    FakeModel model;
    model.start();
    auto agent = make_agent(model);
    CHECK(!agent->submit(MaiSendPrompt{"ses_nope", "hi"}).isOk());
    CHECK(!agent->submit(MaiInterrupt{"ses_nope"}).isOk());
}

void test_no_llm_configured() {
    // M1 的空转服务端就是这个配置：没有模型客户端，但不能崩
    MaiAgent agent(makeMaiMemoryStore(), nullptr);
    Recorder rec;
    rec.attach(agent);
    const std::string sid = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    CHECK(agent.submit(MaiSendPrompt{sid, "hi"}).isOk());
    agent.waitIdle();
    CHECK(rec.count(MaiEventType::SessionError) == 1);
    CHECK(rec.count(MaiEventType::SessionIdle) == 1);
}

void test_concurrent_sessions() {
    FakeModel model;
    model.delayMilliseconds = 40;
    model.start();
    auto agent = make_agent(model);

    // 多个会话必须能同时跑——一个卡住不能拖累其它的
    std::vector<std::string> sids;
    for (int i = 0; i < 4; ++i)
        sids.push_back(agent->submit(MaiCreateSession{"/tmp", "", ""}).value());
    for (const auto& sid : sids) CHECK(agent->submit(MaiSendPrompt{sid, "并发测试"}).isOk());
    agent->waitIdle();
    for (const auto& sid : sids) {
        const auto msgs = agent->listMessages(sid);
        CHECK(msgs.size() == 2);
        if (msgs.size() == 2 && !msgs[1].parts.empty()) {
            const auto* t = std::get_if<MaiTextPart>(&msgs[1].parts[0].body);
            CHECK(t && t->text == "你好，我是测试模型");
        }
    }
}

}  // namespace

int main() {
    test_full_turn();
    test_request_body_is_correct();
    test_multi_turn_history();
    test_reasoning_goes_to_its_own_part();
    test_interrupt();
    test_busy_session_rejects_second_turn();
    test_unknown_session();
    test_no_llm_configured();
    test_concurrent_sessions();
    if (failures == 0) std::printf("loop tests passed\n");
    return failures == 0 ? 0 : 1;
}
