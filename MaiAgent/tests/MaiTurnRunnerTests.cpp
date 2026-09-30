// agent loop 的测试：从 submit(Prompt) 到事件流吐完的完整一圈。
//
// 用假的 Chat Completions 服务端当模型，所以不需要 API key、不花钱、结果完全确定。
// 真实 GLM 说的是同一个协议，切过去只改 baseUrl。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "MaiAgent.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"
#include "MaiFakeModelClient.h"

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
// 默认应答：分三片吐出来，好让上层收到多条 delta。
//
// 不再起假 HTTP 服务端。这个文件测的是"一轮对话的生命周期"——事件顺序、part 归属、落库内容、中断、
// 并发——没有一条和传输层有关。
// 以前每条断言都要绕一圈 JSON -> TCP -> curl -> SSE 解析，想验证
// "第二轮带上了上一轮的回答"还得去 body["messages"][1]["content"] 里翻，
// 而那句话本来就在 MaiModelRequest 这个结构体里放着。
//
// 传输层归 MaiModelClientTests（走真 socket）和 e2e（走真 bridge）管。
const std::vector<std::string> kDefaultChunks{"Hi", ", I am ", "a fake model"};
const char* kDefaultText = "Hi, I am a fake model";

MaiFakeModelClient::Turn defaultTurn(int chunkDelayMilliseconds = 0) {
    MaiFakeModelClient::Turn turn;
    turn.textChunks = kDefaultChunks;
    turn.chunkDelayMilliseconds = chunkDelayMilliseconds;
    return turn;
}

class GatedStreamModel final : public MaiModelClient {
public:
    std::atomic<bool> emitted{false};
    std::atomic<bool> release{false};

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        for (int index = 0; index < 16; ++index) {
            if (sink.onReasoning) sink.onReasoning(std::string(512, 'r'));
            if (sink.onText) sink.onText(std::string(256, 't'));
        }
        emitted.store(true);
        while (!release.load() && !cancel.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cancel.load() ? MaiError::make(MaiErrorCode::Canceled, "canceled by user")
                             : MaiError::ok();
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
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

    // 把某个 part 上的所有 delta 按顺序拼起来——这正是界面做的事，拼出来应该等于完整正文。
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

// 假模型的指针挂在 agent 的生命周期上：agent 持有 unique_ptr，这里交回去的只是借来看的观察指针。
struct AgentUnderTest {
    std::unique_ptr<MaiAgent> agent;
    MaiFakeModelClient* model = nullptr;
};

AgentUnderTest makeAgent(MaiFakeModelClient::Turn repeating, MaiAgent::Options options = {}) {
    auto model = std::make_unique<MaiFakeModelClient>();
    model->setRepeatingTurn(std::move(repeating));
    MaiFakeModelClient* observer = model.get();
    if (options.defaultModel.empty()) options.defaultModel = "glm-5.3";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), nullptr, options),
            observer};
}

void test_prompt_images_reach_the_current_user_message() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(
        MaiSendPrompt{sessionId, "inspect this image", {{"attachments/photo.png", "image/png"}}});
    agent->waitIdle();

    const MaiModelRequest request = underTest.model->lastRequest();
    CHECK(request.workingDirectory == "/tmp");
    CHECK(request.messages.size() == 1);
    if (request.messages.size() == 1) {
        CHECK(request.messages[0].role == MaiModelRole::User);
        CHECK(request.messages[0].images.size() == 1);
        if (request.messages[0].images.size() == 1) {
            CHECK(request.messages[0].images[0].path == "attachments/photo.png");
            CHECK(request.messages[0].images[0].mimeType == "image/png");
        }
    }
}

// ── 用例 ────────────────────────────────────────────────────────

void test_full_turn() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    const std::string msg_id = agent->submit(MaiSendPrompt{sessionId, "who are you?"}).value();
    CHECK(!msg_id.empty());
    CHECK(msg_id.rfind("msg_", 0) == 0);

    agent->waitIdle();

    // 1. 事件顺序：至少要有 delta，最后要有 idle
    CHECK(recorder.count(MaiEventType::MessagePartDelta) == 3);  // 三个 chunk
    CHECK(recorder.count(MaiEventType::SessionIdle) == 1);
    CHECK(recorder.count(MaiEventType::SessionError) == 0);

    // 2. part id 全程稳定 —— 换 id 会让界面重绘甚至闪屏
    const auto all = recorder.snapshot();
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
    CHECK(recorder.assemble(partId) == kDefaultText);

    // 4. 落库的内容一致，刷新后不会变样
    const auto msgs = agent->listMessages(sessionId);
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
            if (t) CHECK(t->text == kDefaultText);
        }
    }

    // 5. 标题自动从第一句话来，不再是"新会话"
    MaiSession s;
    CHECK(agent->getSession(sessionId, s));
    CHECK(s.title == "who are you?");
}

// 注意这里只管"组装出来的请求对不对"。它怎么被序列化成 OpenAI 的线格式（stream=true、
// role 的字符串拼法等）是另一层的事，归 MaiModelClientTests，那边走真 socket，
// 能看到真正发出去的字节。
void test_request_is_assembled_correctly() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", "glm-4.6"}).value();
    agent->submit(MaiSendPrompt{sessionId, "first"});
    agent->waitIdle();

    const MaiModelRequest request = model->lastRequest();
    CHECK(request.model == "glm-4.6");  // 会话上的 model 覆盖默认值
    CHECK(request.messages.size() == 1);
    if (request.messages.size() == 1) {
        CHECK(request.messages[0].role == MaiModelRole::User);
        CHECK(request.messages[0].content == "first");
    }
}

void test_agent_options_supply_system_prompt() {
    auto model = std::make_unique<MaiFakeModelClient>();
    model->setRepeatingTurn(defaultTurn());
    MaiFakeModelClient* observer = model.get();
    MaiAgent::Options options;
    options.baseInstructions = "Return valid GitHub Flavored Markdown.";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr, options);

    const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "show a table"});
    agent.waitIdle();

    const MaiModelRequest request = observer->lastRequest();
    CHECK(request.baseInstructions == options.baseInstructions);
    CHECK(request.messages.size() == 1);
    if (request.messages.size() == 1) {
        CHECK(request.messages[0].role == MaiModelRole::User);
        CHECK(request.messages[0].content == "show a table");
    }
}

void test_multi_turn_history() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "first"});
    agent->waitIdle();
    agent->submit(MaiSendPrompt{sessionId, "second"});
    agent->waitIdle();

    // 第二轮必须带上完整历史，否则模型没有上下文
    const MaiModelRequest request = model->lastRequest();
    CHECK(request.messages.size() == 3);  // user + assistant + user
    if (request.messages.size() == 3) {
        CHECK(request.messages[0].content == "first");
        CHECK(request.messages[1].role == MaiModelRole::Assistant);
        CHECK(request.messages[1].content == kDefaultText);
        CHECK(request.messages[2].content == "second");
    }
    CHECK(agent->listMessages(sessionId).size() == 4);
}

void test_reasoning_goes_to_its_own_part() {
    MaiFakeModelClient::Turn turn = defaultTurn();
    turn.reasoning = "let me think";
    auto underTest = makeAgent(turn);
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "compute this"});
    agent->waitIdle();

    // reasoning 和 text 必须是两个不同的 part，界面才能分开显示
    const auto all = recorder.snapshot();
    std::vector<std::string> part_ids;
    for (const auto& e : all) {
        if (e.type != MaiEventType::MessagePartDelta) continue;
        if (std::find(part_ids.begin(), part_ids.end(), e.partId) == part_ids.end())
            part_ids.push_back(e.partId);
    }
    CHECK(part_ids.size() == 2);

    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) {
        CHECK(msgs[1].parts.size() == 2);
        if (msgs[1].parts.size() == 2) {
            CHECK(std::get_if<MaiReasoningPart>(&msgs[1].parts[0].body) != nullptr);
            CHECK(std::get_if<MaiTextPart>(&msgs[1].parts[1].body) != nullptr);
        }
    }

    // reasoning 不该回灌给模型——它是草稿，会污染下一轮上下文
    agent->submit(MaiSendPrompt{sessionId, "go on"});
    agent->waitIdle();
    bool leaked = false;
    for (const auto& message : model->lastRequest().messages)
        if (message.content.find("let me think") != std::string::npos) leaked = true;
    CHECK(!leaked);
}

void test_interrupt() {
    MaiFakeModelClient::Turn turn;
    turn.textChunks = {"a", "b", "c", "d", "e", "f", "g", "h"};
    turn.chunkDelayMilliseconds = 150;  // 慢慢吐，好让我们插进去
    auto underTest = makeAgent(turn);
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "count"});

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(agent->isBusy(sessionId));
    CHECK(agent->submit(MaiInterrupt{sessionId}).isOk());

    agent->waitIdle();
    CHECK(!agent->isBusy(sessionId));

    // 中断不算错误，而且已经吐出来的内容要保住
    CHECK(recorder.count(MaiEventType::SessionError) == 0);
    CHECK(recorder.count(MaiEventType::SessionIdle) == 1);
    const std::size_t got = recorder.count(MaiEventType::MessagePartDelta);
    CHECK(got > 0);
    CHECK(got < 8);  // 确实提前停了
    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) CHECK(!msgs[1].parts.empty());  // 半截内容也落库
}

void test_busy_session_rejects_second_turn() {
    auto underTest = makeAgent(defaultTurn(120));
    MaiAgent* agent = underTest.agent.get();

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    CHECK(agent->submit(MaiSendPrompt{sessionId, "first"}).isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // 不排队而是拒绝：排队会让用户以为消息丢了，界面上看不出区别
    CHECK(!agent->submit(MaiSendPrompt{sessionId, "second"}).isOk());
    agent->waitIdle();
    CHECK(agent->listMessages(sessionId).size() == 2);
}

void test_unknown_session() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    CHECK(!agent->submit(MaiSendPrompt{"ses_nope", "hi"}).isOk());
    CHECK(!agent->submit(MaiInterrupt{"ses_nope"}).isOk());
}

void test_no_llm_configured() {
    // M1 的空转服务端就是这个配置：没有模型客户端，但不能崩
    MaiAgent agent(makeMaiMemoryStore(), nullptr);
    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    CHECK(agent.submit(MaiSendPrompt{sessionId, "hi"}).isOk());
    agent.waitIdle();
    CHECK(recorder.count(MaiEventType::SessionError) == 1);
    CHECK(recorder.count(MaiEventType::SessionIdle) == 1);
}

void test_concurrent_sessions() {
    auto underTest = makeAgent(defaultTurn(40));
    MaiAgent* agent = underTest.agent.get();

    // 多个会话必须能同时跑——一个卡住不能拖累其它的
    std::vector<std::string> sids;
    for (int i = 0; i < 4; ++i)
        sids.push_back(agent->submit(MaiCreateSession{"/tmp", "", ""}).value());
    for (std::size_t index = 0; index < sids.size(); ++index)
        CHECK(agent->submit(MaiSendPrompt{sids[index], "concurrency " + std::to_string(index)})
                  .isOk());
    agent->waitIdle();
    std::set<std::string> requestPrompts;
    for (std::size_t index = 0; index < underTest.model->requestCount(); ++index) {
        const MaiModelRequest request = underTest.model->request(index);
        CHECK(request.messages.size() == 1);
        if (request.messages.size() == 1) requestPrompts.insert(request.messages.front().content);
    }
    CHECK(requestPrompts.size() == sids.size());
    for (const auto& sessionId : sids) {
        const auto msgs = agent->listMessages(sessionId);
        CHECK(msgs.size() == 2);
        if (msgs.size() == 2 && !msgs[1].parts.empty()) {
            const auto* t = std::get_if<MaiTextPart>(&msgs[1].parts[0].body);
            CHECK(t && t->text == kDefaultText);
        }
    }
}

void test_tool_completion_is_stored_before_update_event() {
    MaiFakeModelClient::Turn first;
    first.invocations.push_back(MaiToolInvocation{"call_1", "missing_tool", "{}"});
    MaiFakeModelClient::Turn second;
    second.textChunks = {"done"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, second});
    MaiAgent agent(makeMaiMemoryStore(), std::move(model));
    const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    std::vector<MaiToolState> observed;
    agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.sessionId != sessionId || event.type != MaiEventType::MessagePartUpdated) return;
        for (const MaiMessage& message : agent.listMessages(sessionId)) {
            for (const MaiMessagePart& part : message.parts) {
                if (part.id != event.partId) continue;
                if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                    observed.push_back(tool->state);
            }
        }
    });

    CHECK(agent.submit(MaiSendPrompt{sessionId, "try a missing tool"}).isOk());
    agent.waitIdle();
    CHECK(observed.size() == 2);
    if (observed.size() == 2) {
        CHECK(observed[0] == MaiToolState::Running);
        CHECK(observed[1] == MaiToolState::Error);
    }
}

void test_streamed_text_is_checkpointed_before_network_finishes() {
    const auto runCase = [](std::unique_ptr<MaiSessionStore> store) {
        auto model = std::make_unique<GatedStreamModel>();
        GatedStreamModel* gated = model.get();
        MaiAgent agent(std::move(store), std::move(model));
        const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
        CHECK(agent.submit(MaiSendPrompt{sessionId, "long answer"}).isOk());
        for (int i = 0; i < 200 && !gated->emitted.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(gated->emitted.load());

        const auto sizes = [&] {
            std::size_t reasoningBytes = 0;
            std::size_t textBytes = 0;
            for (const MaiMessage& message : agent.listMessages(sessionId)) {
                if (message.role != MaiRole::Assistant) continue;
                for (const MaiMessagePart& part : message.parts) {
                    if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body))
                        reasoningBytes += reasoning->text.size();
                    if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                        textBytes += text->text.size();
                }
            }
            return std::pair<std::size_t, std::size_t>{reasoningBytes, textBytes};
        };
        const auto during = sizes();
        CHECK(during.first >= 7 * 1024);
        CHECK(during.second >= 3 * 1024);
        gated->release.store(true);
        agent.waitIdle();
        const auto final = sizes();
        CHECK(final.first == 8 * 1024);
        CHECK(final.second == 4 * 1024);
    };

    runCase(makeMaiMemoryStore());
    auto opened = makeMaiSqliteStore(":memory:");
    CHECK(opened.isOk());
    if (opened.isOk()) runCase(std::move(opened.value()));
}

}  // namespace

int main() {
    test_full_turn();
    test_prompt_images_reach_the_current_user_message();
    test_request_is_assembled_correctly();
    test_agent_options_supply_system_prompt();
    test_multi_turn_history();
    test_reasoning_goes_to_its_own_part();
    test_interrupt();
    test_busy_session_rejects_second_turn();
    test_unknown_session();
    test_no_llm_configured();
    test_concurrent_sessions();
    test_tool_completion_is_stored_before_update_event();
    test_streamed_text_is_checkpointed_before_network_finishes();
    if (failures == 0) std::printf("loop tests passed\n");
    return failures == 0 ? 0 : 1;
}
