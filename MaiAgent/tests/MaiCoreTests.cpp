// 文件作用：验证“MaiCore”的正确行为和边界条件。
// 验证入口：test_id_prefix_and_monotonic、test_session_crud_and_events、
// test_media_only_prompt_has_no_placeholder_text、test_mixed_media_prompt_is_one_message。
// 检查可观察结果而非实现细节。
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <set>
#include <variant>
#include <vector>

#include <json.hpp>

#include "MaiBlockingCheck.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"
#include "MaiAgent.h"
#include "MaiMemoryStore.h"
#include "MaiFakeModelClient.h"
#include "MaiFileSystem.h"
#include "MaiSessionTools.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void test_id_prefix_and_monotonic() {
    CHECK(MaiIdGenerator::newSessionId().rfind("ses_", 0) == 0);
    CHECK(MaiIdGenerator::newMessageId().rfind("msg_", 0) == 0);
    CHECK(MaiIdGenerator::newPartId().rfind("prt_", 0) == 0);
    CHECK(MaiIdGenerator::newEventId().rfind("evt_", 0) == 0);

    // 同一毫秒内连发，必须仍然严格递增且不重复——消息顺序依赖它。
    std::string prev;
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i) {
        const std::string cur = MaiIdGenerator::newMessageId();
        CHECK(seen.insert(cur).second);
        if (!prev.empty()) CHECK(cur > prev);
        prev = cur;
    }
}

static void test_session_crud_and_events() {
    MaiAgent agent(makeMaiMemoryStore(), nullptr, nullptr);

    int created = 0, updated = 0, deleted = 0;
    agent.eventBus().subscribe([&](const MaiEvent& e) {
        if (e.type == MaiEventType::SessionCreated) ++created;
        if (e.type == MaiEventType::SessionUpdated) ++updated;
        if (e.type == MaiEventType::SessionDeleted) ++deleted;
    });

    const std::string a = agent.submit(MaiCreateSession{"/tmp/a", "Session A", "glm-5.3"}).value();
    CHECK(!a.empty());
    CHECK(created == 1);

    MaiSession s;
    CHECK(agent.getSession(a, s));
    CHECK(s.title == "Session A");
    CHECK(s.model == "glm-5.3");
    CHECK(s.agent == "build");

    agent.submit(MaiUpdateSession{a, "Renamed", "", "", "/tmp/moved"});
    CHECK(updated == 1);
    CHECK(agent.getSession(a, s));
    CHECK(s.title == "Renamed");
    CHECK(s.directory == "/tmp/moved");

    const std::string b = agent.submit(MaiCreateSession{"", "", ""}).value();
    CHECK(agent.getSession(b, s));
    CHECK(s.title == kMaiDefaultSessionTitle);

    // 列表按 updated 倒序
    const auto list = agent.listSessions();
    CHECK(list.size() == 2);
    CHECK(list[0].updated >= list[1].updated);

    agent.submit(MaiDeleteSession{a});
    CHECK(deleted == 1);
    CHECK(!agent.getSession(a, s));
    CHECK(agent.listSessions().size() == 1);

    // 删不存在的不该发事件
    agent.submit(MaiDeleteSession{"ses_nope"});
    CHECK(deleted == 1);
}

static void test_model_session_tools_use_the_existing_session_operations() {
    MaiFakeModelClient::Turn create;
    create.invocations.push_back(
        MaiToolInvocation{"call_create", "session_create", R"({"title":"Another topic"})"});
    MaiFakeModelClient::Turn answer;
    answer.textChunks = {"Created."};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{create, answer});
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*tools);
    MaiTool* listTool = tools->find("session_list");
    MaiTool* createTool = tools->find("session_create");
    MaiTool* deleteTool = tools->find("session_delete");
    MaiTool* workspaceTool = tools->find("session_set_workspace");
    CHECK(listTool != nullptr);
    CHECK(createTool != nullptr);
    CHECK(deleteTool != nullptr);
    CHECK(workspaceTool != nullptr);
    if (listTool == nullptr || createTool == nullptr || deleteTool == nullptr ||
        workspaceTool == nullptr)
        return;
    CHECK(deleteTool->requiresApproval(R"({"session_id":"ses_target"})"));
    CHECK(deleteTool->requiresPerCallApproval(R"({"session_id":"ses_target"})"));
    CHECK(workspaceTool->requiresApproval("{}"));
    CHECK(workspaceTool->requiresPerCallApproval("{}"));

    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const std::string current =
        agent.submit(MaiCreateSession{"/tmp/workspace", "Current", "test-model"}).value();
    CHECK(agent.submit(MaiSendPrompt{current, "Create another conversation"}));
    agent.waitIdle();
    const auto sessions = agent.listSessions();
    CHECK(sessions.size() == 2);
    std::string created;
    for (const MaiSession& session : sessions) {
        if (session.id == current) continue;
        created = session.id;
        CHECK(session.title == "Another topic");
        CHECK(session.directory == "/tmp/workspace");
        CHECK(session.model == "test-model");
    }
    CHECK(!created.empty());
    if (created.empty()) return;

    MaiToolContext context;
    context.sessionAgent = &agent;
    context.sessionId = current;
    const std::string chosenDirectory = MaiFileSystem::temporaryDirectory().toUtf8();
    const std::string switchArguments =
        nlohmann::json{{"session_id", current}, {"workspace_path", chosenDirectory}}.dump();
    const std::size_t sessionCountBeforeSwitch = agent.listSessions().size();
    const MaiToolResult relativeWorkspace = workspaceTool->execute(
        nlohmann::json{{"session_id", current}, {"workspace_path", "."}}.dump(), context);
    CHECK(relativeWorkspace.hasError());
    const MaiToolResult switched = workspaceTool->execute(switchArguments, context);
    CHECK(!switched.hasError());
    MaiSession switchedSession;
    CHECK(agent.getSession(current, switchedSession));
    CHECK(switchedSession.directory ==
          MaiFileSystem::resolve(MaiFilePath::fromUtf8(chosenDirectory)).toUtf8());
    CHECK(agent.listSessions().size() == sessionCountBeforeSwitch);
    if (!switched.hasError()) {
        const auto result = nlohmann::json::parse(switched.output());
        CHECK(result.at("session_id") == current);
        CHECK(result.at("changed") == true);
    }
    const MaiToolResult unchanged = workspaceTool->execute(switchArguments, context);
    CHECK(!unchanged.hasError());
    if (!unchanged.hasError())
        CHECK(nlohmann::json::parse(unchanged.output()).at("changed") == false);
    auto desktopCreate =
        makeMaiCreateSessionTool(MaiSessionCreateMode::DefaultOrExplicitDirectory, chosenDirectory);
    CHECK(!desktopCreate->requiresApproval("{}"));
    const MaiToolResult missingDirectory =
        desktopCreate->execute(R"({"title":"Needs a chosen folder"})", context);
    CHECK(!missingDirectory.hasError());
    if (!missingDirectory.hasError()) {
        const std::string id =
            nlohmann::json::parse(missingDirectory.output()).at("session_id").get<std::string>();
        MaiSession defaultSession;
        CHECK(agent.getSession(id, defaultSession));
        CHECK(defaultSession.directory ==
              MaiFileSystem::resolve(MaiFilePath::fromUtf8(chosenDirectory)).toUtf8());
        CHECK(agent.submit(MaiDeleteSession{id}));
    }
    CHECK(agent.listSessions().size() == 2);
    const MaiToolResult relativeDirectory = desktopCreate->execute(R"({"directory":"."})", context);
    CHECK(relativeDirectory.hasError());
    const std::string desktopArguments =
        nlohmann::json{{"directory", chosenDirectory}, {"title", "Chosen folder"}}.dump();
    CHECK(desktopCreate->requiresApproval(desktopArguments));
    CHECK(desktopCreate->requiresPerCallApproval(desktopArguments));
    const MaiToolResult desktopCreated = desktopCreate->execute(desktopArguments, context);
    CHECK(!desktopCreated.hasError());
    if (!desktopCreated.hasError()) {
        const std::string id =
            nlohmann::json::parse(desktopCreated.output()).at("session_id").get<std::string>();
        MaiSession desktopSession;
        CHECK(agent.getSession(id, desktopSession));
        CHECK(desktopSession.directory ==
              MaiFileSystem::resolve(MaiFilePath::fromUtf8(chosenDirectory)).toUtf8());
        CHECK(agent.submit(MaiDeleteSession{id}));
    }
    const MaiToolResult invalidTitle = createTool->execute(R"({"title":"bad\nname"})", context);
    CHECK(invalidTitle.hasError());
    const MaiToolResult listed = listTool->execute("{}", context);
    CHECK(!listed.hasError());
    if (!listed.hasError()) {
        const auto parsed = nlohmann::json::parse(listed.output());
        CHECK(parsed.at("sessions").size() == 2);
    }
    const MaiToolResult selfDelete =
        deleteTool->execute(nlohmann::json{{"session_id", current}}.dump(), context);
    CHECK(selfDelete.hasError());
    CHECK(selfDelete.error().code() == MaiErrorCode::Busy);
    const MaiToolResult deleted =
        deleteTool->execute(nlohmann::json{{"session_id", created}}.dump(), context);
    CHECK(!deleted.hasError());
    MaiSession removed;
    CHECK(!agent.getSession(created, removed));
    CHECK(agent.listSessions().size() == 1);
}

static void test_session_workspace_tool_effective_next_turn() {
    MaiFakeModelClient::Turn first;
    first.textChunks = {"working"};
    first.chunkDelayMilliseconds = 1000;
    MaiFakeModelClient::Turn second;
    second.textChunks = {"done"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, second});
    MaiFakeModelClient* observed = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*tools);
    MaiTool* workspaceTool = tools->find("session_set_workspace");
    CHECK(workspaceTool != nullptr);
    if (workspaceTool == nullptr) return;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const std::string current = agent.submit(MaiCreateSession{"/tmp", "Current", ""}).value();
    MaiToolContext context;
    context.sessionAgent = &agent;
    context.sessionId = current;
    const std::string target = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(agent.submit(MaiSendPrompt{current, "Start"}));
    CHECK(agent.isBusy(current));
    // isBusy 只说明 worker 已登记，不保证它已读取本轮工作区。先等真实模型请求到达，
    // 再验证修改不会影响已经发出的请求，避免把线程调度时序当成下一轮生效的契约。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observed->requestCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(observed->requestCount() == 1);
    const MaiToolResult switched = workspaceTool->execute(
        nlohmann::json{{"session_id", current}, {"workspace_path", target}}.dump(), context);
    CHECK(!switched.hasError());
    if (!switched.hasError()) {
        const auto output = nlohmann::json::parse(switched.output());
        CHECK(output.at("effective_next_turn") == true);
        CHECK(output.at("session_id") == current);
    }
    agent.waitIdle();
    CHECK(observed->requestCount() == 1);
    CHECK(observed->request(0).workingDirectory == "/tmp");
    CHECK(agent.submit(MaiSendPrompt{current, "Continue"}));
    agent.waitIdle();
    CHECK(observed->requestCount() == 2);
    CHECK(observed->request(1).workingDirectory ==
          MaiFileSystem::resolve(MaiFilePath::fromUtf8(target)).toUtf8());
    CHECK(agent.listSessions().size() == 1);
}

static void test_media_only_prompt_has_no_placeholder_text() {
    MaiFakeModelClient::Turn reply;
    reply.textChunks = {"received"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{reply, reply});
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr);
    const std::string session = agent.submit(MaiCreateSession{"/tmp", "", "test-model"}).value();
    CHECK(!agent.submit(MaiSendPrompt{session, ""}));
    CHECK(agent.submit(MaiSendPrompt{session, "", {{"photo.jpg", "image/jpeg"}}}));
    agent.waitIdle();
    CHECK(agent.submit(MaiSendPrompt{session, "", {}, {{"clip.mp4", "video/mp4"}}}));
    agent.waitIdle();
    const auto messages = agent.listMessages(session);
    int mediaOnly = 0;
    for (const auto& message : messages) {
        if (message.role != MaiRole::User) continue;
        CHECK(message.text().empty());
        CHECK(message.parts.size() == 1);
        if (message.parts.size() == 1 &&
            (std::holds_alternative<MaiImagePart>(message.parts[0].body) ||
             std::holds_alternative<MaiVideoPart>(message.parts[0].body)))
            ++mediaOnly;
    }
    CHECK(mediaOnly == 2);
}

static void test_mixed_media_prompt_is_one_message() {
    MaiFakeModelClient::Turn reply;
    reply.textChunks = {"received"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{reply, reply});
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr);
    const std::string session = agent.submit(MaiCreateSession{"/tmp", "", "test-model"}).value();
    CHECK(agent.submit(MaiSendPrompt{
        session, "Describe both", {{"photo.jpg", "image/jpeg"}}, {{"clip.mp4", "video/mp4"}}}));
    agent.waitIdle();
    CHECK(agent.submit(
        MaiSendPrompt{session, "", {{"other.jpg", "image/jpeg"}}, {{"other.mp4", "video/mp4"}}}));
    agent.waitIdle();

    int describedMessages = 0;
    int mediaOnlyMessages = 0;
    for (const auto& message : agent.listMessages(session)) {
        if (message.role != MaiRole::User) continue;
        const auto& parts = message.parts;
        if (message.text() == "Describe both") {
            ++describedMessages;
            CHECK(parts.size() == 3);
            CHECK(std::holds_alternative<MaiTextPart>(parts[0].body));
            CHECK(std::get<MaiImagePart>(parts[1].body).path == "photo.jpg");
            CHECK(std::get<MaiVideoPart>(parts[2].body).path == "clip.mp4");
        } else {
            ++mediaOnlyMessages;
            CHECK(message.text().empty());
            CHECK(parts.size() == 2);
            CHECK(std::get<MaiImagePart>(parts[0].body).path == "other.jpg");
            CHECK(std::get<MaiVideoPart>(parts[1].body).path == "other.mp4");
        }
    }
    CHECK(describedMessages == 1);
    CHECK(mediaOnlyMessages == 1);
}

static void test_quoted_message_is_scoped_and_persisted() {
    MaiFakeModelClient::Turn reply;
    reply.textChunks = {"received"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{reply, reply});
    MaiFakeModelClient* observer = model.get();
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr);
    const std::string session = agent.submit(MaiCreateSession{"/tmp", "", "test-model"}).value();
    const std::string other = agent.submit(MaiCreateSession{"/tmp", "", "test-model"}).value();
    CHECK(agent.submit(MaiSendPrompt{session, "A video", {}, {{"clips/exact.mp4", "video/mp4"}}}));
    agent.waitIdle();
    std::string sourceId;
    for (const auto& message : agent.listMessages(session))
        if (message.role == MaiRole::User) sourceId = message.id;
    CHECK(!sourceId.empty());
    CHECK(!agent.submit(MaiSendPrompt{other, "Save it", {}, {}, sourceId}));
    CHECK(agent.submit(MaiSendPrompt{session, "Save it", {}, {}, sourceId}));
    agent.waitIdle();
    bool storedQuote = false;
    for (const auto& message : agent.listMessages(session)) {
        if (message.role != MaiRole::User || message.id == sourceId) continue;
        for (const auto& part : message.parts) {
            const auto* quote = std::get_if<MaiQuotePart>(&part.body);
            if (quote && quote->messageId == sourceId) storedQuote = true;
        }
    }
    CHECK(storedQuote);
    CHECK(observer->requestCount() >= 2);
    bool exactPathInCurrentRequest = false;
    for (const auto& message : observer->request(1).messages) {
        if (message.role == MaiModelRole::User &&
            message.content.find("clips/exact.mp4") != std::string::npos &&
            message.content.find(sourceId) != std::string::npos)
            exactPathInCurrentRequest = true;
    }
    CHECK(exactPathInCurrentRequest);
}

static void test_interrupted_tool_is_recovered_on_startup() {
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "ses_recovery";
    session.directory = "/tmp";
    store->putSession(session);

    MaiMessage message;
    message.id = "msg_recovery";
    message.role = MaiRole::Assistant;
    MaiToolPart running;
    running.tool = "glob";
    running.callId = "call_recovery";
    running.state = MaiToolState::Running;
    message.parts.push_back(MaiMessagePart{"prt_running", running});
    MaiToolPart completed = running;
    completed.state = MaiToolState::Completed;
    completed.output = "done";
    message.parts.push_back(MaiMessagePart{"prt_completed", completed});
    store->putMessage(session.id, message);

    MaiAgent agent(std::move(store), nullptr, nullptr);
    const auto messages = agent.listMessages(session.id);
    CHECK(messages.size() == 1);
    if (messages.empty()) return;
    const auto* recovered = std::get_if<MaiToolPart>(&messages.front().parts[0].body);
    const auto* unchanged = std::get_if<MaiToolPart>(&messages.front().parts[1].body);
    CHECK(recovered != nullptr && recovered->state == MaiToolState::Error);
    CHECK(recovered != nullptr && recovered->output.find("interrupted") != std::string::npos);
    CHECK(unchanged != nullptr && unchanged->state == MaiToolState::Completed);
    CHECK(unchanged != nullptr && unchanged->output == "done");
    CHECK(messages.front().completed != 0);
}

// 事件处理函数里不许做慢活——而且这条要真的被装上，不能只是注释。
//
// 不能直接测"违反了会炸"：那会终止进程，测试框架接不住。所以退一步，测**守卫确实在生效**：
// 处理函数里问一句"现在允许慢活吗"，答案必须是否。
// 这就足以说明 MaiFileSystem 里那几个断言会在这条路径上被触发。
static void test_event_handlers_run_with_blocking_disallowed() {
    MaiEventBus bus;

    bool allowedInsideHandler = true;
    bus.subscribe([&](const MaiEvent&) { allowedInsideHandler = maiIsBlockingAllowed(); });

    // 发之前是允许的：工作线程本来就要读文件、写库
    CHECK(maiIsBlockingAllowed());

    MaiEvent event;
    event.type = MaiEventType::SessionIdle;
    bus.publish(event);

    CHECK(!allowedInsideHandler);
    // 发完要恢复，不能把整个线程一直摁住
    CHECK(maiIsBlockingAllowed());
}

static void test_blocking_scopes_nest() {
    CHECK(maiIsBlockingAllowed());
    {
        MaiScopedDisallowBlocking outer;
        CHECK(!maiIsBlockingAllowed());
        {
            // 明确开一个口子
            MaiScopedAllowBlocking inner;
            CHECK(maiIsBlockingAllowed());
        }
        // 内层退出时要恢复成**进来之前**的状态，不是无条件放开——无条件放开的话，
        // 一次嵌套就把外层的限制悄悄拆了。
        CHECK(!maiIsBlockingAllowed());
    }
    CHECK(maiIsBlockingAllowed());
}

static void test_thread_name() {
    MaiThread::setCurrentName("mai-test");
    CHECK(MaiThread::currentName() == "mai-test");

    // 别的线程互不影响（thread_local）
    std::string fromOtherThread = "not set";
    std::thread worker([&] {
        fromOtherThread = MaiThread::currentName();
        MaiThread::setCurrentName("mai-other");
    });
    worker.join();
    CHECK(fromOtherThread.empty());
    CHECK(MaiThread::currentName() == "mai-test");
}

static void test_unsubscribe() {
    MaiAgent agent(makeMaiMemoryStore(), nullptr, nullptr);
    int n = 0;
    const auto tok = agent.eventBus().subscribe([&](const MaiEvent&) { ++n; });
    agent.submit(MaiCreateSession{"", "x", ""});
    CHECK(n == 1);
    agent.eventBus().unsubscribe(tok);
    agent.submit(MaiCreateSession{"", "y", ""});
    CHECK(n == 1);
    CHECK(agent.eventBus().subscriberCount() == 0);
}

int main() {
    test_id_prefix_and_monotonic();
    test_session_crud_and_events();
    test_model_session_tools_use_the_existing_session_operations();
    test_session_workspace_tool_effective_next_turn();
    test_media_only_prompt_has_no_placeholder_text();
    test_mixed_media_prompt_is_one_message();
    test_quoted_message_is_scoped_and_persisted();
    test_interrupted_tool_is_recovered_on_startup();
    test_event_handlers_run_with_blocking_disallowed();
    test_blocking_scopes_nest();
    test_thread_name();
    test_unsubscribe();
    if (failures == 0) std::printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
