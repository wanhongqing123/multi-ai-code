// 工具循环测试：模型说要调工具 → 我们执行 → 结果回灌 → 它接着答。
//
// 这是 agent 之所以是 agent 的地方，也是 M3 唯一真正需要证明的东西。
// 假模型按脚本依次返回"要调工具"和"最终回答"，所以整个闭环是确定的。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiAgent.h"

using nlohmann::json;

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

// 一次模型应答的剧本：要么吐文本，要么发起工具调用。
struct Script {
    std::string text;
    std::string tool_name;
    std::string tool_args;
};

struct FakeModel {
    httplib::Server server;
    std::thread th;
    int port = 0;

    std::mutex mu;
    std::vector<Script> scripts;  // 第 N 次请求用第 N 个剧本
    std::vector<std::string> bodies;
    std::size_t served = 0;

    void start() {
        server.Post("/chat/completions", [this](const httplib::Request& request,
                                                httplib::Response& response) {
            Script s;
            {
                std::lock_guard<std::mutex> lock(mu);
                bodies.push_back(request.body);
                if (served < scripts.size()) s = scripts[served];
                ++served;
            }
            std::string out;
            if (!s.tool_name.empty()) {
                json tc;
                tc["index"] = 0;
                tc["id"] = "call_" + s.tool_name;
                tc["function"] = json{{"name", s.tool_name}, {"arguments", s.tool_args}};
                json d;
                d["tool_calls"] = json::array({tc});
                json c;
                c["delta"] = std::move(d);
                json root;
                root["choices"] = json::array({c});
                out += "data: " + root.dump() + "\n\n";
            }
            if (!s.text.empty()) {
                json d;
                d["content"] = s.text;
                json c;
                c["delta"] = std::move(d);
                json root;
                root["choices"] = json::array({c});
                out += "data: " + root.dump() + "\n\n";
            }
            out += "data: [DONE]\n\n";
            response.set_content(out, "text/event-stream");
        });
        port = server.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeModel() {
        server.stop();
        if (th.joinable()) th.join();
    }

    std::string base() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    std::size_t requestCount() {
        std::lock_guard<std::mutex> lock(mu);
        return bodies.size();
    }
    json body(std::size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        if (i >= bodies.size()) return json::object();
        return json::parse(bodies[i], nullptr, false);
    }
};

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

std::unique_ptr<MaiAgent> make_agent(const FakeModel& model) {
    MaiModelConfig config;
    config.baseUrl = model.base();
    config.apiKey = "test";
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);
    MaiAgent::Options options;
    options.defaultModel = "glm-5.3";
    return std::make_unique<MaiAgent>(makeMaiMemoryStore(), makeMaiModelClient(config),
                                      std::move(tools), options);
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
    FakeModel model;
    // 第一次：要调 read。第二次：拿到内容后给出回答。
    model.scripts = {
        Script{"", "read", R"({"path":"src/hello.txt"})"},
        Script{"The file has two lines.", "", ""},
    };
    model.start();

    auto agent = make_agent(model);
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent->waitIdle();

    // 1. 模型被请求了两次——工具循环确实转了一圈
    CHECK(model.requestCount() == 2);

    // 2. 第一次请求里带了工具清单
    const json first = model.body(0);
    CHECK(first.contains("tools"));
    if (first.contains("tools")) {
        CHECK(first["tools"].size() == 4);
        bool has_read = false;
        for (const auto& t : first["tools"])
            if (t["function"]["name"] == "read") has_read = true;
        CHECK(has_read);
    }

    // 3. 第二次请求里必须带着调用和结果，而且 toolCallId 对得上——
    //    对不上的话模型认不出这是哪次调用的结果，下一轮会重复调。
    const json second = model.body(1);
    CHECK(second.contains("messages"));
    bool foundAssistantCall = false;
    bool foundToolResult = false;
    std::string callId;
    for (const auto& m : second["messages"]) {
        if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
            foundAssistantCall = true;
            callId = m["tool_calls"][0].value("id", "");
            CHECK(m["tool_calls"][0]["function"]["name"] == "read");
        }
        if (m.value("role", "") == "tool") {
            foundToolResult = true;
            CHECK(m.value("toolCallId", "") == callId);
            // 真的把文件内容回灌了
            CHECK(m.value("content", std::string{}).find("line one") != std::string::npos);
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

void test_tool_error_is_fed_back() {
    Workspace workspace;
    FakeModel model;
    // 模型要读一个不存在的文件，然后（拿到错误后）改口
    model.scripts = {
        Script{"", "read", R"({"path":"no-such-file.txt"})"},
        Script{"That file does not exist.", "", ""},
    };
    model.start();

    auto agent = make_agent(model);
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "read it"});
    agent->waitIdle();

    CHECK(model.requestCount() == 2);
    // 错误必须回灌——模型要知道失败了才能换个做法，而不是干等
    const json second = model.body(1);
    bool fedError = false;
    for (const auto& m : second["messages"])
        if (m.value("role", "") == "tool" &&
            m.value("content", std::string{}).find("does not exist") != std::string::npos)
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
    FakeModel model;
    // 模型编了一个不存在的工具名——这事真会发生
    model.scripts = {
        Script{"", "made-up-tool", R"({})"},
        Script{"Sorry, I used the wrong tool.", "", ""},
    };
    model.start();

    auto agent = make_agent(model);
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "do something"});
    agent->waitIdle();

    // 整轮不能因此失败，而是把"没这个工具"告诉模型让它改
    CHECK(model.requestCount() == 2);
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
    FakeModel model;
    // 模型（或提示词注入）让它读工作目录外的东西
    model.scripts = {
        Script{"", "read", R"({"path":"../../../etc/passwd"})"},
        Script{"I cannot read that path.", "", ""},
    };
    model.start();

    auto agent = make_agent(model);
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
    FakeModel model;
    // 模型一直要调工具，永不收手——真实中会发生（它会绕圈）
    for (int i = 0; i < 40; ++i)
        model.scripts.push_back(Script{"", "read", R"({"path":"src/hello.txt"})"});
    model.start();

    MaiModelConfig config;
    config.baseUrl = model.base();
    config.apiKey = "test";
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);
    MaiAgent::Options options;
    options.defaultModel = "glm-5.3";
    options.maxToolIterations = 3;  // 调小便于测试
    MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config), std::move(tools), options);

    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "loop forever"});
    agent.waitIdle();

    // 到上限就停，不能无限烧钱
    CHECK(model.requestCount() == 3);
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
    FakeModel model;
    model.scripts = {Script{"just chatting", "", ""}};
    model.start();

    MaiModelConfig config;
    config.baseUrl = model.base();
    config.apiKey = "test";
    // 不给工具注册表 = 纯对话模式
    MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config), nullptr, {});
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "chat"});
    agent.waitIdle();

    // 请求里不该出现 tools 字段——模型看不到工具就不会尝试调用
    const json b = model.body(0);
    CHECK(!b.contains("tools"));
}

}  // namespace

int main() {
    test_tool_loop_closes();
    test_tool_error_is_fed_back();
    test_unknown_tool_does_not_kill_the_turn();
    test_path_escape_through_model();
    test_iteration_cap();
    test_no_tools_means_no_tool_field();
    if (failures == 0) std::printf("tool loop tests passed\n");
    return failures == 0 ? 0 : 1;
}
