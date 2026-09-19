// 权限闸门测试。
//
// 分两层：
//   1. 闸门本身——阻塞 / 唤醒 / 中断 / 超时 / 兜底方向，不需要搭 agent。
//   2. 接进 agent 之后的真实路径——模型要写文件，闸门拦住，用户点头才写。
//
// 第 2 层才是真正要证的东西：**闸门装上了，而且真的拦得住**。只测第 1 层的话，
// 一个忘了调 checkPermission 的 bug 会一路绿灯过去。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MaiAgent.h"
#include "MaiFakeModelClient.h"
#include "MaiPermission.h"

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

const std::atomic<bool> kNeverCancel{false};

MaiPermissionRequest makeRequest(const std::string& id, const std::string& sessionId,
                                 const std::string& tool = "write") {
    MaiPermissionRequest r;
    r.id = id;
    r.sessionId = sessionId;
    r.messageId = "msg_1";
    r.partId = "prt_1";
    r.toolName = tool;
    r.arguments = R"({"path":"a.txt"})";
    return r;
}

// 等一个条件成立，最多等 limit。轮询而不是睡固定时长：睡固定时长的测试在慢机器上会假失败，
// 在快机器上白白拖慢整套。
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// ── 第 1 层：闸门本身 ───────────────────────────────────────────

void test_decision_parsing() {
    MaiPermissionDecision d = MaiPermissionDecision::Approved;

    // 线上取值照 codex 的 ReviewDecision，snake_case。
    CHECK(maiParsePermissionDecision("approved", d) && d == MaiPermissionDecision::Approved);
    CHECK(maiParsePermissionDecision("approved_for_session", d) &&
          d == MaiPermissionDecision::ApprovedForSession);
    CHECK(maiParsePermissionDecision("denied", d) && d == MaiPermissionDecision::Denied);

    // 认不出来必须返回 false，且**不能**把 out 改成 Approved。把拼错的输入当放行，
    // 等于闸门被一个错别字拆掉，而且毫无痕迹。
    d = MaiPermissionDecision::Denied;
    CHECK(!maiParsePermissionDecision("Approved", d));  // 大小写不认
    CHECK(!maiParsePermissionDecision("once", d));      // 改名前的旧取值也不认
    CHECK(!maiParsePermissionDecision("allow", d));
    CHECK(!maiParsePermissionDecision("yes", d));
    CHECK(!maiParsePermissionDecision("", d));

    // timed_out 是闸门自己的结论，不接受从线上传进来——允许调用方声称"超时了"，
    // 等于给了它一条绕过用户的路。
    CHECK(!maiParsePermissionDecision("timed_out", d));

    CHECK(d == MaiPermissionDecision::Denied);  // 全程没被动过
}

void test_ask_blocks_until_reply() {
    MaiPermissionGate gate;
    std::atomic<bool> announced{false};
    std::atomic<int> outcome{-1};

    std::thread worker([&] {
        const auto d = gate.ask(
            makeRequest("per_1", "ses_1"), [&](const MaiPermissionRequest&) { announced = true; },
            kNeverCancel);
        outcome = static_cast<int>(d);
    });

    CHECK(waitFor([&] { return announced.load(); }));
    // 广播出去之后、裁决之前，请求必须已经在待办表里——顺序反了的话，
    // 抢在中间到达的裁决会找不到这条记录。
    CHECK(gate.listPending().size() == 1);
    CHECK(outcome.load() == -1);  // 还在等

    CHECK(gate.reply("per_1", MaiPermissionDecision::Approved));
    worker.join();

    CHECK(outcome.load() == static_cast<int>(MaiPermissionDecision::Approved));
    CHECK(gate.listPending().empty());
}

void test_reply_to_unknown_id() {
    MaiPermissionGate gate;
    // 界面重复点、或者对着已经结束的请求点，都走这里。不是异常。
    CHECK(!gate.reply("per_nope", MaiPermissionDecision::Approved));
}

void test_reject_and_always() {
    MaiPermissionGate gate;

    std::atomic<int> first{-1};
    std::thread t1([&] {
        first = static_cast<int>(gate.ask(makeRequest("per_1", "ses_1"), nullptr, kNeverCancel));
    });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));
    gate.reply("per_1", MaiPermissionDecision::Denied);
    t1.join();
    CHECK(first.load() == static_cast<int>(MaiPermissionDecision::Denied));
    CHECK(!gate.isAllowedInSession("ses_1", "write"));

    std::thread t2([&] { gate.ask(makeRequest("per_2", "ses_1"), nullptr, kNeverCancel); });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));
    gate.reply("per_2", MaiPermissionDecision::ApprovedForSession);
    t2.join();

    CHECK(gate.isAllowedInSession("ses_1", "write"));
    CHECK(!gate.isAllowedInSession("ses_1", "shell"));  // 只对那一个工具
    CHECK(!gate.isAllowedInSession("ses_2", "write"));  // 只对那一个会话

    // 会话删掉之后授权不该留着：重建一个同 id 的会话会白捡上一个的授权。
    gate.forgetSession("ses_1");
    CHECK(!gate.isAllowedInSession("ses_1", "write"));
}

void test_cancel_session_wakes_waiter() {
    MaiPermissionGate gate;
    std::atomic<int> outcome{-1};
    std::thread worker([&] {
        outcome = static_cast<int>(gate.ask(makeRequest("per_1", "ses_1"), nullptr, kNeverCancel));
    });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));

    gate.cancelSession("ses_2");  // 别的会话，不该动这一条
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(outcome.load() == -1);

    gate.cancelSession("ses_1");
    worker.join();
    // 没人点头就醒了，一律按拒绝。兜底成允许 = 没人点头也能改用户的文件。
    CHECK(outcome.load() == static_cast<int>(MaiPermissionDecision::Denied));
}

void test_cancel_flag_wakes_waiter() {
    // 这条测的是兜底轮询：只翻 cancel 标志、不调 cancelSession，等待方也必须能醒。
    // agent 析构时走的就是这条路。
    MaiPermissionGate gate;
    std::atomic<bool> cancel{false};
    std::atomic<int> outcome{-1};
    std::thread worker([&] {
        outcome = static_cast<int>(gate.ask(makeRequest("per_1", "ses_1"), nullptr, cancel));
    });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));

    cancel = true;
    worker.join();
    CHECK(outcome.load() == static_cast<int>(MaiPermissionDecision::Denied));
}

void test_timeout() {
    MaiPermissionGate::Options options;
    options.timeoutMs = 300;
    MaiPermissionGate gate(options);

    const auto started = std::chrono::steady_clock::now();
    const auto d = gate.ask(makeRequest("per_1", "ses_1"), nullptr, kNeverCancel);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    // 超时**不是** Denied：没人看过那个请求，说成"用户拒绝了"是对模型撒谎，它会照着"换个做法"去试，
    // 而真相是没人在，换什么做法都一样没人批。
    CHECK(d == MaiPermissionDecision::TimedOut);
    CHECK(elapsed >= 250);  // 确实等过
    CHECK(elapsed < 3000);  // 但没等到天荒地老
    CHECK(gate.listPending().empty());
}

// ── 第 2 层：接进 agent 的真实路径 ──────────────────────────────

// 一次应答的剧本：要么吐一句话，要么发起一次工具调用。
//
// 这里不再起假 HTTP 服务端。理由见 MaiFakeModelClient.h——要测的是闸门拦不拦得住，
// 不是 SSE 解析对不对，中间那层传输纯属噪音。
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

// 回灌给模型的工具结果。拒绝时模型看到的就是这段文字。
//
// 以前得把最后一个请求体的 JSON 解出来、倒着找 role=="tool"；
// 现在直接在结构体里找 MaiModelRole::ToolResult，编译器帮着查类型。
std::string lastToolResultText(const MaiFakeModelClient& model) {
    const MaiModelRequest request = model.lastRequest();
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
        if (it->role == MaiModelRole::ToolResult) return it->content;
    }
    return {};
}

struct Workspace {
    fs::path root;
    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-perm-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string utf8Root() const {
        return root.u8string();
    }
    bool has(const char* name) const {
        std::error_code ec;
        return fs::exists(root / name, ec);
    }
};

// 建一个装好内置工具的 agent，并把假模型的指针交回去给用例回看。
//
// 指针的生命周期挂在 agent 上：agent 持有 unique_ptr，用例只借着看。
struct AgentUnderTest {
    std::unique_ptr<MaiAgent> agent;
    MaiFakeModelClient* model = nullptr;  // agent 持有，这里只是观察用
};

AgentUnderTest makeAgent(std::vector<MaiFakeModelClient::Turn> script) {
    auto model = std::make_unique<MaiFakeModelClient>(std::move(script));
    MaiFakeModelClient* observer = model.get();

    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);

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
    std::size_t count(MaiEventType t) {
        std::lock_guard<std::mutex> lock(mu);
        std::size_t n = 0;
        for (const auto& e : events)
            if (e.type == t) ++n;
        return n;
    }
};

// 取当前 assistant 消息里那个工具 part 的状态。
bool toolPartState(MaiAgent& agent, const std::string& sessionId, MaiToolState& out) {
    for (const auto& m : agent.listMessages(sessionId)) {
        for (const auto& p : m.parts) {
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) {
                out = t->state;
                return true;
            }
        }
    }
    return false;
}

void test_write_waits_for_approval_then_runs() {
    Workspace workspace;
    auto underTest =
        makeAgent({callTurn("write", R"({"path":"note.txt","content":"only after approval"})"),
                   sayTurn("Written.")});
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write note.txt"});

    // 闸门必须先拦住：这一刻文件不该存在。
    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    CHECK(!workspace.has("note.txt"));

    const auto pending = agent->listPendingPermissions();
    CHECK(pending.size() == 1);
    CHECK(pending[0].toolName == "write");
    CHECK(pending[0].sessionId == sessionId);
    CHECK(pending[0].arguments.find("note.txt") != std::string::npos);
    CHECK(pending[0].id.rfind("per_", 0) == 0);

    // 等授权期间，工具卡应当是 Pending——界面靠这个显示"等待授权"。
    MaiToolState state = MaiToolState::Completed;
    CHECK(toolPartState(*agent, sessionId, state));
    CHECK(state == MaiToolState::Pending);

    // permission.asked 带了 permissionId 和 partId，界面据此定位到那张卡。
    bool sawAsked = false;
    for (const auto& e : recorder.all()) {
        if (e.type != MaiEventType::PermissionAsked) continue;
        sawAsked = true;
        CHECK(e.permissionId == pending[0].id);
        CHECK(e.partId == pending[0].partId);
        CHECK(e.sessionId == sessionId);
    }
    CHECK(sawAsked);

    MaiReplyPermission reply;
    reply.permissionId = pending[0].id;
    reply.decision = MaiPermissionDecision::Approved;
    CHECK(agent->submit(reply).isOk());

    agent->waitIdle();

    CHECK(workspace.has("note.txt"));
    std::ifstream in(workspace.root / "note.txt", std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), {});
    CHECK(content == "only after approval");

    CHECK(toolPartState(*agent, sessionId, state));
    CHECK(state == MaiToolState::Completed);
    CHECK(recorder.count(MaiEventType::PermissionReplied) == 1);
    CHECK(agent->listPendingPermissions().empty());
}

void test_reject_blocks_write_and_tells_model() {
    Workspace workspace;
    auto underTest =
        makeAgent({callTurn("write", R"({"path":"nope.txt","content":"must not be written"})"),
                   sayTurn("All right, I will not write it.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write nope.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::Denied;
    CHECK(agent->submit(reply).isOk());

    agent->waitIdle();

    // 最要紧的一条：文件没被写出来。
    CHECK(!workspace.has("nope.txt"));

    MaiToolState state = MaiToolState::Completed;
    CHECK(toolPartState(*agent, sessionId, state));
    CHECK(state == MaiToolState::Error);

    // 模型得知道发生了什么，而且得被明确告知别重试——否则它会拿同样的参数把 12 圈烧光。
    const std::string toolResultText = lastToolResultText(*model);
    CHECK(toolResultText.find("denied") != std::string::npos);
    CHECK(toolResultText.find("Do not retry") != std::string::npos);
}

void test_rejected_repeat_does_not_ask_again() {
    Workspace workspace;
    // 模型被拒之后原样再调一次——真实模型经常这么干。
    const char* args = R"({"path":"again.txt","content":"x"})";
    auto underTest = makeAgent({callTurn("write", args, "call_1"),
                                callTurn("write", args, "call_2"), sayTurn("Never mind.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write again.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::Denied;
    agent->submit(reply);

    agent->waitIdle();

    CHECK(!workspace.has("again.txt"));
    // 第二次同样的调用直接回同样的拒绝，**不再弹第二个框**。不做这件事的话，
    // 模型每重试一次用户就要点一次"不行"。
    CHECK(recorder.count(MaiEventType::PermissionAsked) == 1);
    CHECK(model->requestCount() == 3);  // 三圈都跑到了，没卡住
}

void test_always_in_session_asks_only_once() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("write", R"({"path":"a.txt","content":"1"})", "call_1"),
                                callTurn("write", R"({"path":"b.txt","content":"2"})", "call_2"),
                                sayTurn("Both files written.")});
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write two files"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::ApprovedForSession;
    agent->submit(reply);

    agent->waitIdle();

    CHECK(workspace.has("a.txt"));
    CHECK(workspace.has("b.txt"));  // 第二次没再问，直接放行
    CHECK(recorder.count(MaiEventType::PermissionAsked) == 1);
}

// 清空聊天记录**不能**把"本会话都允许"一起清掉。
//
// 这就是 MaiClearMessages 存在的全部理由。界面上"清空重来"如果实现成
// 删会话再建一个，授权记录会跟着 MaiDeleteSession 一起没（那边明确调了
// forgetSession）。用户感觉不到自己丢了什么，只会发现它又开始一个一个问了——
// 而他明明点过"本会话都允许"。
void test_clearing_history_keeps_the_session_grant() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("write", R"({"path":"a.txt","content":"1"})", "call_1"),
                                sayTurn("Wrote a.txt."),
                                callTurn("write", R"({"path":"b.txt","content":"2"})", "call_2"),
                                sayTurn("Wrote b.txt.")});
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();

    // 第一轮：问一次，用户选"本会话都允许"。
    agent->submit(MaiSendPrompt{sessionId, "write a.txt"});
    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::ApprovedForSession;
    agent->submit(reply);
    agent->waitIdle();
    CHECK(workspace.has("a.txt"));
    CHECK(recorder.count(MaiEventType::PermissionAsked) == 1);

    // 清空聊天记录。
    CHECK(agent->submit(MaiClearMessages{sessionId}).isOk());
    CHECK(agent->listMessages(sessionId).empty());
    // 会话还在——换了 id 的话界面上那个固定的 AI 助手就认不出来了。
    MaiSession session;
    CHECK(agent->getSession(sessionId, session));
    // 标题一起清掉，让下一轮重新起名。留着的话头部会挂着一句已经不存在的对话。
    CHECK(session.title.empty());

    // 第二轮：**不该再问**。授权记录在闸门里，不在聊天记录里。
    agent->submit(MaiSendPrompt{sessionId, "write b.txt"});

    // 这里刻意**不用 waitIdle()**。授权记录要是丢了，这一轮会停在等授权上
    // 永远不结束（permissionTimeoutMs = 0 是无限等），waitIdle 也就永远不返回——
    // 用例变成挂死而不是报错。挂死比失败难查得多：看不到断言、看不到行号，
    // 只有一个卡着不动的进程。有界等待会在超时后干脆地红掉。
    CHECK(waitFor([&] { return workspace.has("b.txt"); }));
    CHECK(recorder.count(MaiEventType::PermissionAsked) == 1);

    // 收尾：万一它真的又问了（说明这个用例红了），把请求答掉再退出。
    //
    // 不能只靠 waitIdle()——那一轮正卡在等人裁决上，而这个 agent 是无限等的，
    // waitIdle 会一直不返回，用例变成挂死。挂死比失败难查得多：
    // 没有断言、没有行号，只有一个卡着不动的进程。
    for (const MaiPermissionRequest& pending : agent->listPendingPermissions()) {
        agent->submit(MaiReplyPermission{pending.id, MaiPermissionDecision::Denied});
    }
    agent->waitIdle();
}

// 正在跑的时候不让清。
//
// 清了也没用：那一轮的 assistant 消息还在往库里写，下一次 putMessage 会把它塞回来，
// 最后剩一条来历不明的半截记录。界面该先 MaiInterrupt 再清。
void test_clearing_a_busy_session_is_refused() {
    Workspace workspace;
    auto underTest =
        makeAgent({callTurn("write", R"({"path":"slow.txt","content":"x"})"), sayTurn("done")});
    MaiAgent* agent = underTest.agent.get();

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write slow.txt"});

    // 卡在等授权 = 这一轮还在跑。
    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));

    MaiResult<std::string> cleared = agent->submit(MaiClearMessages{sessionId});
    CHECK(!cleared.isOk());
    CHECK(cleared.error().code() == MaiErrorCode::Busy);
    // 消息一条都没少。
    CHECK(!agent->listMessages(sessionId).empty());

    // 收尾，别让 agent 析构时还挂着一个等授权的轮次。
    agent->submit(MaiInterrupt{sessionId});
    agent->waitIdle();
}

void test_read_never_asks() {
    Workspace workspace;
    std::ofstream(workspace.root / "x.txt", std::ios::binary) << "some content";
    auto underTest = makeAgent({callTurn("read", R"({"path":"x.txt"})"), sayTurn("Got it.")});
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "read x.txt"});
    agent->waitIdle();

    // 只读的工具不该打扰用户。每一次多余的确认都在训练用户闭眼点"允许"。
    CHECK(recorder.count(MaiEventType::PermissionAsked) == 0);
    CHECK(agent->listPendingPermissions().empty());
}

void test_interrupt_while_waiting_for_approval() {
    Workspace workspace;
    auto underTest = makeAgent(
        {callTurn("write", R"({"path":"interrupted.txt","content":"x"})"), sayTurn("Understood.")});
    MaiAgent* agent = underTest.agent.get();
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write interrupted.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    CHECK(agent->submit(MaiInterrupt{sessionId}).isOk());

    // 卡在等授权的那个线程必须醒过来，否则这一句永远返回不了。
    agent->waitIdle();

    CHECK(!workspace.has("interrupted.txt"));
    CHECK(agent->listPendingPermissions().empty());
    CHECK(!agent->isBusy(sessionId));
}

void test_reply_to_stale_permission_is_not_found() {
    Workspace workspace;
    auto underTest =
        makeAgent({callTurn("write", R"({"path":"stale.txt","content":"x"})"), sayTurn("Done.")});
    MaiAgent* agent = underTest.agent.get();
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "write stale.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    const std::string pid = agent->listPendingPermissions()[0].id;

    MaiReplyPermission reply;
    reply.permissionId = pid;
    reply.decision = MaiPermissionDecision::Approved;
    CHECK(agent->submit(reply).isOk());
    agent->waitIdle();

    // 同一个 id 再点一次：界面重复点击、或者两个端同时点，都会走到这里。必须是明确的 NotFound，
    // 不能假装成功——界面要据此把对话框收掉。
    const auto again = agent->submit(reply);
    CHECK(!again.isOk());
    CHECK(again.error().code() == MaiErrorCode::NotFound);

    MaiReplyPermission empty;
    CHECK(agent->submit(empty).error().code() == MaiErrorCode::InvalidInput);
}

}  // namespace

int main() {
    std::printf("-> test_decision_parsing\n");
    test_decision_parsing();
    std::printf("-> test_ask_blocks_until_reply\n");
    test_ask_blocks_until_reply();
    std::printf("-> test_reply_to_unknown_id\n");
    test_reply_to_unknown_id();
    std::printf("-> test_reject_and_always\n");
    test_reject_and_always();
    std::printf("-> test_cancel_session_wakes_waiter\n");
    test_cancel_session_wakes_waiter();
    std::printf("-> test_cancel_flag_wakes_waiter\n");
    test_cancel_flag_wakes_waiter();
    std::printf("-> test_timeout\n");
    test_timeout();

    std::printf("-> test_write_waits_for_approval_then_runs\n");
    test_write_waits_for_approval_then_runs();
    std::printf("-> test_reject_blocks_write_and_tells_model\n");
    test_reject_blocks_write_and_tells_model();
    std::printf("-> test_rejected_repeat_does_not_ask_again\n");
    test_rejected_repeat_does_not_ask_again();
    std::printf("-> test_always_in_session_asks_only_once\n");
    test_always_in_session_asks_only_once();
    std::printf("-> test_clearing_history_keeps_the_session_grant\n");
    test_clearing_history_keeps_the_session_grant();
    std::printf("-> test_clearing_a_busy_session_is_refused\n");
    test_clearing_a_busy_session_is_refused();
    std::printf("-> test_read_never_asks\n");
    test_read_never_asks();
    std::printf("-> test_interrupt_while_waiting_for_approval\n");
    test_interrupt_while_waiting_for_approval();
    std::printf("-> test_reply_to_stale_permission_is_not_found\n");
    test_reply_to_stale_permission_is_not_found();

    if (failures) {
        std::printf("\n%d checks failed\n", failures);
        return 1;
    }
    std::printf("\npermission tests passed\n");
    return 0;
}
