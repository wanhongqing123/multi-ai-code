// 工具循环测试：模型说要调工具 → 我们执行 → 结果回灌 → 它接着答。
//
// 这是 agent 之所以是 agent 的地方，也是 M3 唯一真正需要证明的东西。
// 假模型按脚本依次返回"要调工具"和"最终回答"，所以整个闭环是确定的。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "MaiAgent.h"
#include "MaiMemoryStore.h"
#include "MaiFileTools.h"
#include "MaiFakeModelClient.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 一次模型应答的剧本：要么吐一句话，要么发起一次工具调用。
//
// 不起假 HTTP 服务端。这个文件测的是工具循环本身——调用发出去、执行、结果回灌、
// 再问一遍——中间那层传输是噪音。以前想断言"第二次请求带着调用和结果、而且 toolCallId 对得上"，
// 得去 JSON 里翻`messages[i]["tool_calls"][0]["id"]`；现在直接看`message.invocations[0].id`，
// 编译器帮着查类型。
//
// tool_calls 在线上怎么嵌套（function.name 那一层）是线格式的事，归 MaiModelClientTests 管，
// 那边走真 socket。
MaiFakeModelClient::Turn sayTurn(const std::string& text) {
    MaiFakeModelClient::Turn turn;
    turn.textChunks = {text};
    return turn;
}

MaiFakeModelClient::Turn callTurn(const std::string& tool, const std::string& arguments,
                                  const std::string& callId = "call_1") {
    MaiFakeModelClient::Turn turn;
    turn.invocations.push_back(MaiToolInvocation{callId, tool, arguments});
    return turn;
}

MaiFakeModelClient::Turn reasoningTurn(const std::string& text) {
    MaiFakeModelClient::Turn turn;
    turn.reasoning = text;
    return turn;
}

struct Workspace {
    fs::path root;
    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-loop-" + std::to_string(std::rand()));
        fs::create_directories(root / "src");
        std::ofstream(root / "src" / "hello.txt", std::ios::binary) << "line one\nline two\n";
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    // 注意：不要用 root / "中文"，MSVC 会按 ANSI 代码页解释字面量。
    std::string utf8Root() const {
        return root.u8string();
    }
};

struct AgentUnderTest {
    std::unique_ptr<MaiAgent> agent;
    MaiFakeModelClient* model = nullptr;  // agent 持有，这里只借着看
};

class ImageResultTool final : public MaiTool {
public:
    explicit ImageResultTool(std::string path) : mPath(std::move(path)) {}

    std::string name() const override {
        return "screenshot";
    }
    std::string description() const override {
        return "Capture the current display.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        if (context.model != "glm-5.3") {
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "tool did not receive the active model name");
        }
        return MaiToolResult::successWithImages("Captured the current display.",
                                                {MaiToolImage{mPath, "image/png"}});
    }

private:
    std::string mPath;
};

class TextResultTool final : public MaiTool {
public:
    std::string name() const override {
        return "status";
    }
    std::string description() const override {
        return "Return a status value.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success("ready");
    }
};

AgentUnderTest makeAgent(std::vector<MaiFakeModelClient::Turn> script,
                         MaiAgent::Options options = {}, bool withTools = true) {
    auto model = std::make_unique<MaiFakeModelClient>(std::move(script));
    MaiFakeModelClient* observer = model.get();

    std::unique_ptr<MaiToolRegistry> tools;
    if (withTools) {
        tools = std::make_unique<MaiToolRegistry>();
        registerMaiBuiltinTools(*tools);
    }
    if (options.defaultModel.empty()) options.defaultModel = "glm-5.3";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), std::move(tools),
                                       options),
            observer};
}

AgentUnderTest makeAgentWithImageTool(std::vector<MaiFakeModelClient::Turn> script,
                                      const std::string& imagePath) {
    auto model = std::make_unique<MaiFakeModelClient>(std::move(script));
    MaiFakeModelClient* observer = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<ImageResultTool>(imagePath));
    tools->add(std::make_unique<TextResultTool>());
    MaiAgent::Options options;
    options.defaultModel = "glm-5.3";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), std::move(tools),
                                       options),
            observer};
}

struct Recorder {
    std::mutex mu;
    std::vector<MaiEvent> events;
    void attach(MaiAgent& a) {
        a.eventBus().subscribe([this](const MaiEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
        });
    }
    std::vector<MaiEvent> all() {
        std::lock_guard<std::mutex> lock(mu);
        return events;
    }
};

// ── 用例 ────────────────────────────────────────────────────────

void test_tool_loop_closes() {
    Workspace workspace;
    // 第一次：要调 read。第二次：拿到内容后给出回答。
    auto underTest = makeAgent({callTurn("read", R"({"path":"src/hello.txt"})", "call_read"),
                                sayTurn("The file has two lines.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent->waitIdle();

    // 1. 模型被请求了两次——工具循环确实转了一圈
    CHECK(model->requestCount() == 2);

    // 2. 第一次请求里带了工具清单
    const MaiModelRequest first = model->request(0);
    // 按名字查而不是比个数：加一个工具不该让这条用例红。
    CHECK(!first.tools.empty());
    bool hasRead = false;
    for (const auto& tool : first.tools)
        if (tool.name == "read") hasRead = true;
    CHECK(hasRead);

    // 3. 第二次请求里必须带着调用和结果，而且 toolCallId 对得上——
    //    对不上的话模型认不出这是哪次调用的结果，下一轮会重复调。
    const MaiModelRequest second = model->request(1);
    bool foundAssistantCall = false;
    bool foundToolResult = false;
    std::string callId;
    for (const auto& message : second.messages) {
        if (message.role == MaiModelRole::Assistant && !message.invocations.empty()) {
            foundAssistantCall = true;
            callId = message.invocations[0].id;
            CHECK(message.invocations[0].name == "read");
        }
        if (message.role == MaiModelRole::ToolResult) {
            foundToolResult = true;
            CHECK(message.toolCallId == callId);
            // 真的把文件内容回灌了
            CHECK(message.content.find("line one") != std::string::npos);
        }
    }
    CHECK(foundAssistantCall);
    CHECK(foundToolResult);

    // 4. 落库里有一个完成状态的 tool part
    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);
    const MaiToolPart* toolPart = nullptr;
    const MaiTextPart* final_text = nullptr;
    if (msgs.size() == 2) {
        for (const auto& p : msgs[1].parts) {
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) toolPart = t;
            if (const auto* t = std::get_if<MaiTextPart>(&p.body)) final_text = t;
        }
    }
    CHECK(toolPart != nullptr);
    if (toolPart) {
        CHECK(toolPart->tool == "read");
        CHECK(toolPart->state == MaiToolState::Completed);
        CHECK(toolPart->output.find("line one") != std::string::npos);
    }
    // 5. 最终回答也在
    CHECK(final_text != nullptr);
    if (final_text) CHECK(final_text->text == "The file has two lines.");

    // 6. 界面能看到工具卡的状态变化
    std::size_t partUpdates = 0;
    for (const auto& e : recorder.all())
        if (e.type == MaiEventType::MessagePartUpdated) ++partUpdates;
    CHECK(partUpdates >= 2);  // 至少 running 和 completed 各一次
}

void test_tool_image_is_persisted_and_fed_back_to_the_model() {
    Workspace workspace;
    const fs::path screenshot = workspace.root / "screenshot.png";
    std::ofstream(screenshot, std::ios::binary) << "png";
    auto underTest = makeAgentWithImageTool(
        {callTurn("screenshot", R"({})", "call_screenshot"), sayTurn("I can see it.")},
        screenshot.u8string());
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();

    agent.submit(MaiSendPrompt{sessionId, "inspect my screen"});
    agent.waitIdle();

    CHECK(model->requestCount() == 2);
    const MaiModelRequest second = model->request(1);
    bool foundToolResult = false;
    bool foundScreenshot = false;
    std::size_t toolResultIndex = 0;
    std::size_t screenshotIndex = 0;
    for (std::size_t i = 0; i < second.messages.size(); ++i) {
        const MaiModelMessage& message = second.messages[i];
        if (message.role == MaiModelRole::ToolResult && message.toolCallId == "call_screenshot") {
            foundToolResult = true;
            toolResultIndex = i;
        }
        if (message.role == MaiModelRole::User && message.images.size() == 1 &&
            message.images.front().path == screenshot.u8string()) {
            foundScreenshot = true;
            screenshotIndex = i;
            CHECK(message.images.front().mimeType == "image/png");
            CHECK(message.content.find("Image returned") != std::string::npos);
        }
    }
    CHECK(foundToolResult);
    CHECK(foundScreenshot);
    CHECK(toolResultIndex < screenshotIndex);

    bool persistedScreenshot = false;
    for (const MaiMessage& message : agent.listMessages(sessionId)) {
        for (const MaiMessagePart& part : message.parts) {
            const auto* image = std::get_if<MaiImagePart>(&part.body);
            if (image && image->path == screenshot.u8string() && image->mimeType == "image/png") {
                persistedScreenshot = true;
            }
        }
    }
    CHECK(persistedScreenshot);
}

void test_tool_image_keeps_parallel_tool_calls_in_one_assistant_batch() {
    Workspace workspace;
    const fs::path screenshot = workspace.root / "batch-screenshot.png";
    std::ofstream(screenshot, std::ios::binary) << "png";
    MaiFakeModelClient::Turn calls;
    calls.invocations = {MaiToolInvocation{"call_screenshot", "screenshot", R"({})"},
                         MaiToolInvocation{"call_status", "status", R"({})"}};
    auto underTest =
        makeAgentWithImageTool({calls, sayTurn("Both results arrived.")}, screenshot.u8string());
    MaiAgent& agent = *underTest.agent;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();

    agent.submit(MaiSendPrompt{sessionId, "inspect and report status"});
    agent.waitIdle();

    const MaiModelRequest second = underTest.model->request(1);
    std::size_t assistantBatches = 0;
    std::size_t toolResults = 0;
    std::size_t imageObservations = 0;
    std::size_t batchIndex = 0;
    std::size_t lastToolResultIndex = 0;
    std::size_t imageIndex = 0;
    for (std::size_t i = 0; i < second.messages.size(); ++i) {
        const MaiModelMessage& message = second.messages[i];
        if (message.role == MaiModelRole::Assistant && !message.invocations.empty()) {
            ++assistantBatches;
            batchIndex = i;
            CHECK(message.invocations.size() == 2);
        } else if (message.role == MaiModelRole::ToolResult) {
            ++toolResults;
            lastToolResultIndex = i;
        } else if (!message.images.empty()) {
            ++imageObservations;
            imageIndex = i;
        }
    }
    CHECK(assistantBatches == 1);
    CHECK(toolResults == 2);
    CHECK(imageObservations == 1);
    CHECK(batchIndex < lastToolResultIndex);
    CHECK(lastToolResultIndex < imageIndex);
}

void test_reasoning_only_after_tools_retries_for_final_answer() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("read", R"({"path":"src/hello.txt"})", "call_read"),
                                reasoningTurn("I should summarize the tool output."),
                                sayTurn("The file has two lines.")});
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent.waitIdle();

    CHECK(model->requestCount() == 3);
    const MaiModelRequest finalRequest = model->request(2);
    CHECK(!finalRequest.messages.empty());
    if (!finalRequest.messages.empty()) {
        const MaiModelMessage& instruction = finalRequest.messages.back();
        CHECK(instruction.role == MaiModelRole::System);
        CHECK(instruction.content.find("complete final answer") != std::string::npos);
    }

    bool foundFinalText = false;
    const auto messages = agent.listMessages(sessionId);
    if (messages.size() == 2) {
        for (const auto& part : messages[1].parts) {
            const auto* text = std::get_if<MaiTextPart>(&part.body);
            if (text && text->text == "The file has two lines.") foundFinalText = true;
        }
    }
    CHECK(foundFinalText);
}

void test_repeated_missing_final_answer_reports_error() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("read", R"({"path":"src/hello.txt"})"),
                                reasoningTurn("I should summarize this."),
                                reasoningTurn("I still did not provide the answer.")});
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent.waitIdle();

    CHECK(model->requestCount() == 3);
    bool foundError = false;
    for (const auto& event : recorder.all()) {
        if (event.type == MaiEventType::SessionError &&
            event.detail.find("without a final answer") != std::string::npos) {
            foundError = true;
        }
    }
    CHECK(foundError);
}

void test_tool_error_is_fed_back() {
    Workspace workspace;
    // 模型要读一个不存在的文件，然后（拿到错误后）改口
    auto underTest = makeAgent(
        {callTurn("read", R"({"path":"no-such-file.txt"})"), sayTurn("That file does not exist.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "read it"});
    agent->waitIdle();

    CHECK(model->requestCount() == 2);
    // 错误必须回灌——模型要知道失败了才能换个做法，而不是干等
    const MaiModelRequest second = model->request(1);
    bool fedError = false;
    for (const auto& message : second.messages)
        if (message.role == MaiModelRole::ToolResult &&
            message.content.find("does not exist") != std::string::npos)
            fedError = true;
    CHECK(fedError);

    const auto msgs = agent->listMessages(sessionId);
    const MaiToolPart* toolPart = nullptr;
    if (msgs.size() == 2)
        for (const auto& p : msgs[1].parts)
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) toolPart = t;
    CHECK(toolPart && toolPart->state == MaiToolState::Error);
}

void test_unknown_tool_does_not_kill_the_turn() {
    Workspace workspace;
    auto underTest =
        makeAgent({callTurn("made-up-tool", R"({})"), sayTurn("Sorry, I used the wrong tool.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "do something"});
    agent->waitIdle();

    // 整轮不能因此失败，而是把"没这个工具"告诉模型让它改
    CHECK(model->requestCount() == 2);
    const auto msgs = agent->listMessages(sessionId);
    bool hasFinalText = false;
    if (msgs.size() == 2)
        for (const auto& p : msgs[1].parts)
            if (const auto* t = std::get_if<MaiTextPart>(&p.body))
                if (t->text.find("wrong tool") != std::string::npos) hasFinalText = true;
    CHECK(hasFinalText);
}

void test_path_escape_through_model() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("read", R"({"path":"../../../etc/passwd"})"),
                                sayTurn("I cannot read that path.")});
    MaiAgent* agent = underTest.agent.get();
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "read the system password file"});
    agent->waitIdle();

    const auto msgs = agent->listMessages(sessionId);
    const MaiToolPart* toolPart = nullptr;
    if (msgs.size() == 2)
        for (const auto& p : msgs[1].parts)
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) toolPart = t;
    CHECK(toolPart != nullptr);
    if (toolPart) {
        CHECK(toolPart->state == MaiToolState::Error);
        CHECK(toolPart->output.find("outside the working directory") != std::string::npos);
    }
}

void test_iteration_cap() {
    Workspace workspace;
    // 模型一直要调工具，永不收手——真实中会发生（它会绕圈）
    std::vector<MaiFakeModelClient::Turn> script;
    for (int i = 0; i < 40; ++i) script.push_back(callTurn("read", R"({"path":"src/hello.txt"})"));

    MaiAgent::Options options;
    options.maxToolIterations = 3;  // 调小便于测试
    auto underTest = makeAgent(std::move(script), options);
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;

    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "loop forever"});
    agent.waitIdle();

    // 到上限就停，不能无限烧钱
    CHECK(model->requestCount() == 3);
    // 而且要明确告诉用户停在哪儿了，不是悄悄结束让人以为跑完了
    bool toldUser = false;
    for (const auto& e : recorder.all())
        if (e.type == MaiEventType::SessionError &&
            e.detail.find("tool-call limit") != std::string::npos)
            toldUser = true;
    CHECK(toldUser);
}

void test_no_tools_means_no_tool_field() {
    Workspace workspace;
    // 不给工具注册表 = 纯对话模式
    auto underTest = makeAgent({sayTurn("just chatting")}, {}, /*withTools=*/false);
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "chat"});
    agent.waitIdle();

    // 请求里不该带工具清单——模型看不到工具就不会尝试调用。
    // 这个清单空着时线上会不会真的省掉 tools 字段，是序列化那一层的事，归 MaiModelClientTests 管。
    CHECK(model->request(0).tools.empty());
}

void test_model_exception_becomes_a_session_error() {
    class ThrowingModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest&, const MaiStreamSink&,
                        const std::atomic<bool>&) override {
            throw std::runtime_error("model exploded");
        }

        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }
    };

    Workspace workspace;
    auto model = std::make_unique<ThrowingModel>();
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr);
    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "trigger the model"});
    agent.waitIdle();

    bool reported = false;
    for (const MaiEvent& event : recorder.all()) {
        if (event.type == MaiEventType::SessionError &&
            event.detail.find("model exploded") != std::string::npos) {
            reported = true;
        }
    }
    CHECK(reported);
}

}  // namespace

int main() {
    test_tool_loop_closes();
    test_tool_image_is_persisted_and_fed_back_to_the_model();
    test_tool_image_keeps_parallel_tool_calls_in_one_assistant_batch();
    test_reasoning_only_after_tools_retries_for_final_answer();
    test_repeated_missing_final_answer_reports_error();
    test_tool_error_is_fed_back();
    test_unknown_tool_does_not_kill_the_turn();
    test_path_escape_through_model();
    test_iteration_cap();
    test_no_tools_means_no_tool_field();
    test_model_exception_becomes_a_session_error();
    if (failures == 0) std::printf("tool loop tests passed\n");
    return failures == 0 ? 0 : 1;
}
