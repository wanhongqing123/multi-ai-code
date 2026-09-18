// 权限闸门测试。
//
// 分两层：
//   1. 闸门本身——阻塞 / 唤醒 / 中断 / 超时 / 兜底方向，不需要搭 agent。
//   2. 接进 agent 之后的真实路径——模型要写文件，闸门拦住，用户点头才写。
//
// 第 2 层才是真正要证的东西：**闸门装上了，而且真的拦得住**。
// 只测第 1 层的话，一个忘了调 checkPermission 的 bug 会一路绿灯过去。
#include <atomic>
#include <chrono>
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
#include "MaiPermission.h"

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

// 等一个条件成立，最多等 limit。轮询而不是睡固定时长：
// 睡固定时长的测试在慢机器上会假失败，在快机器上白白拖慢整套。
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
    MaiPermissionDecision d = MaiPermissionDecision::Once;

    CHECK(maiParsePermissionDecision("once", d) && d == MaiPermissionDecision::Once);
    CHECK(maiParsePermissionDecision("allow", d) && d == MaiPermissionDecision::Once);
    CHECK(maiParsePermissionDecision("always", d) && d == MaiPermissionDecision::AlwaysInSession);
    CHECK(maiParsePermissionDecision("reject", d) && d == MaiPermissionDecision::Reject);
    CHECK(maiParsePermissionDecision("deny", d) && d == MaiPermissionDecision::Reject);

    // 认不出来必须返回 false，且**不能**把 out 改成 Once。
    // 把拼错的输入当放行，等于闸门被一个错别字拆掉，而且毫无痕迹。
    d = MaiPermissionDecision::Reject;
    CHECK(!maiParsePermissionDecision("Once", d));  // 大小写不认
    CHECK(!maiParsePermissionDecision("yes", d));
    CHECK(!maiParsePermissionDecision("", d));
    CHECK(d == MaiPermissionDecision::Reject);  // 没被动过
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
    // 广播出去之后、裁决之前，请求必须已经在待办表里——
    // 顺序反了的话，抢在中间到达的裁决会找不到这条记录。
    CHECK(gate.listPending().size() == 1);
    CHECK(outcome.load() == -1);  // 还在等

    CHECK(gate.reply("per_1", MaiPermissionDecision::Once));
    worker.join();

    CHECK(outcome.load() == static_cast<int>(MaiPermissionDecision::Once));
    CHECK(gate.listPending().empty());
}

void test_reply_to_unknown_id() {
    MaiPermissionGate gate;
    // 界面重复点、或者对着已经结束的请求点，都走这里。不是异常。
    CHECK(!gate.reply("per_nope", MaiPermissionDecision::Once));
}

void test_reject_and_always() {
    MaiPermissionGate gate;

    std::atomic<int> first{-1};
    std::thread t1([&] {
        first = static_cast<int>(gate.ask(makeRequest("per_1", "ses_1"), nullptr, kNeverCancel));
    });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));
    gate.reply("per_1", MaiPermissionDecision::Reject);
    t1.join();
    CHECK(first.load() == static_cast<int>(MaiPermissionDecision::Reject));
    CHECK(!gate.isAllowedInSession("ses_1", "write"));

    std::thread t2([&] { gate.ask(makeRequest("per_2", "ses_1"), nullptr, kNeverCancel); });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));
    gate.reply("per_2", MaiPermissionDecision::AlwaysInSession);
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
    CHECK(outcome.load() == static_cast<int>(MaiPermissionDecision::Reject));
}

void test_cancel_flag_wakes_waiter() {
    // 这条测的是兜底轮询：只翻 cancel 标志、不调 cancelSession，
    // 等待方也必须能醒。agent 析构时走的就是这条路。
    MaiPermissionGate gate;
    std::atomic<bool> cancel{false};
    std::atomic<int> outcome{-1};
    std::thread worker([&] {
        outcome = static_cast<int>(gate.ask(makeRequest("per_1", "ses_1"), nullptr, cancel));
    });
    CHECK(waitFor([&] { return gate.listPending().size() == 1; }));

    cancel = true;
    worker.join();
    CHECK(outcome.load() == static_cast<int>(MaiPermissionDecision::Reject));
}

void test_timeout() {
    MaiPermissionGate::Options opts;
    opts.timeoutMs = 300;
    MaiPermissionGate gate(opts);

    const auto started = std::chrono::steady_clock::now();
    const auto d = gate.ask(makeRequest("per_1", "ses_1"), nullptr, kNeverCancel);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK(d == MaiPermissionDecision::Reject);
    CHECK(elapsed >= 250);  // 确实等过
    CHECK(elapsed < 3000);  // 但没等到天荒地老
    CHECK(gate.listPending().empty());
}

// ── 第 2 层：接进 agent 的真实路径 ──────────────────────────────

struct Script {
    std::string text;
    std::string tool_name;
    std::string tool_args;
};

struct FakeModel {
    httplib::Server srv;
    std::thread th;
    int port = 0;

    std::mutex mu;
    std::vector<Script> scripts;
    std::vector<std::string> bodies;
    std::size_t served = 0;

    void start() {
        srv.Post("/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            Script s;
            {
                std::lock_guard<std::mutex> lock(mu);
                bodies.push_back(req.body);
                if (served < scripts.size()) s = scripts[served];
                ++served;
            }
            std::string out;
            if (!s.tool_name.empty()) {
                json tc;
                tc["index"] = 0;
                tc["id"] = "call_" + std::to_string(bodies.size());
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
            res.set_content(out, "text/event-stream");
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

    std::size_t requestCount() {
        std::lock_guard<std::mutex> lock(mu);
        return bodies.size();
    }

    // 回灌给模型的工具结果。拒绝时模型看到的就是这段文字。
    std::string lastToolResultText() {
        std::lock_guard<std::mutex> lock(mu);
        if (bodies.empty()) return {};
        const json b = json::parse(bodies.back(), nullptr, false);
        if (!b.is_object() || !b.contains("messages")) return {};
        for (auto it = b["messages"].rbegin(); it != b["messages"].rend(); ++it) {
            if (it->value("role", "") == "tool") return it->value("content", "");
        }
        return {};
    }
};

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

std::unique_ptr<MaiAgent> makeAgent(const FakeModel& model) {
    MaiModelConfig cfg;
    cfg.baseUrl = model.base();
    cfg.apiKey = "test";
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);
    MaiAgent::Options opts;
    opts.defaultModel = "glm-5.3";
    return std::make_unique<MaiAgent>(makeMaiMemoryStore(), makeMaiModelClient(cfg),
                                      std::move(tools), opts);
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
bool toolPartState(MaiAgent& agent, const std::string& sid, MaiToolState& out) {
    for (const auto& m : agent.listMessages(sid)) {
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
    Workspace ws;
    FakeModel model;
    model.scripts = {
        Script{"", "write", R"({"path":"note.txt","content":"批准之后才该出现"})"},
        Script{"写好了。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "写个 note.txt"});

    // 闸门必须先拦住：这一刻文件不该存在。
    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    CHECK(!ws.has("note.txt"));

    const auto pending = agent->listPendingPermissions();
    CHECK(pending.size() == 1);
    CHECK(pending[0].toolName == "write");
    CHECK(pending[0].sessionId == sid);
    CHECK(pending[0].arguments.find("note.txt") != std::string::npos);
    CHECK(pending[0].id.rfind("per_", 0) == 0);

    // 等授权期间，工具卡应当是 Pending——界面靠这个显示"等待授权"。
    MaiToolState state = MaiToolState::Completed;
    CHECK(toolPartState(*agent, sid, state));
    CHECK(state == MaiToolState::Pending);

    // permission.asked 带了 permissionId 和 partId，界面据此定位到那张卡。
    bool sawAsked = false;
    for (const auto& e : rec.all()) {
        if (e.type != MaiEventType::PermissionAsked) continue;
        sawAsked = true;
        CHECK(e.permissionId == pending[0].id);
        CHECK(e.partId == pending[0].partId);
        CHECK(e.sessionId == sid);
    }
    CHECK(sawAsked);

    MaiReplyPermission reply;
    reply.permissionId = pending[0].id;
    reply.decision = MaiPermissionDecision::Once;
    CHECK(agent->submit(reply).isOk());

    agent->waitIdle();

    CHECK(ws.has("note.txt"));
    std::ifstream in(ws.root / "note.txt", std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), {});
    CHECK(content == "批准之后才该出现");

    CHECK(toolPartState(*agent, sid, state));
    CHECK(state == MaiToolState::Completed);
    CHECK(rec.count(MaiEventType::PermissionReplied) == 1);
    CHECK(agent->listPendingPermissions().empty());
}

void test_reject_blocks_write_and_tells_model() {
    Workspace ws;
    FakeModel model;
    model.scripts = {
        Script{"", "write", R"({"path":"nope.txt","content":"不该被写出来"})"},
        Script{"好的，我不写了。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "写个 nope.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::Reject;
    CHECK(agent->submit(reply).isOk());

    agent->waitIdle();

    // 最要紧的一条：文件没被写出来。
    CHECK(!ws.has("nope.txt"));

    MaiToolState state = MaiToolState::Completed;
    CHECK(toolPartState(*agent, sid, state));
    CHECK(state == MaiToolState::Error);

    // 模型得知道发生了什么，而且得被明确告知别重试——否则它会拿同样的
    // 参数把 12 圈烧光。
    const std::string fed = model.lastToolResultText();
    CHECK(fed.find("拒绝") != std::string::npos);
    CHECK(fed.find("不要重试") != std::string::npos);
}

void test_rejected_repeat_does_not_ask_again() {
    Workspace ws;
    FakeModel model;
    // 模型被拒之后原样再调一次——真实模型经常这么干。
    const char* args = R"({"path":"again.txt","content":"x"})";
    model.scripts = {
        Script{"", "write", args},
        Script{"", "write", args},
        Script{"算了。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "写 again.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::Reject;
    agent->submit(reply);

    agent->waitIdle();

    CHECK(!ws.has("again.txt"));
    // 第二次同样的调用直接回同样的拒绝，**不再弹第二个框**。
    // 不做这件事的话，模型每重试一次用户就要点一次"不行"。
    CHECK(rec.count(MaiEventType::PermissionAsked) == 1);
    CHECK(model.requestCount() == 3);  // 三圈都跑到了，没卡住
}

void test_always_in_session_asks_only_once() {
    Workspace ws;
    FakeModel model;
    model.scripts = {
        Script{"", "write", R"({"path":"a.txt","content":"1"})"},
        Script{"", "write", R"({"path":"b.txt","content":"2"})"},
        Script{"两个都写好了。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "写两个文件"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    MaiReplyPermission reply;
    reply.permissionId = agent->listPendingPermissions()[0].id;
    reply.decision = MaiPermissionDecision::AlwaysInSession;
    agent->submit(reply);

    agent->waitIdle();

    CHECK(ws.has("a.txt"));
    CHECK(ws.has("b.txt"));  // 第二次没再问，直接放行
    CHECK(rec.count(MaiEventType::PermissionAsked) == 1);
}

void test_read_never_asks() {
    Workspace ws;
    std::ofstream(ws.root / "x.txt", std::ios::binary) << "内容";
    FakeModel model;
    model.scripts = {
        Script{"", "read", R"({"path":"x.txt"})"},
        Script{"读到了。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    Recorder rec;
    rec.attach(*agent);

    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "读 x.txt"});
    agent->waitIdle();

    // 只读的工具不该打扰用户。每一次多余的确认都在训练用户闭眼点"允许"。
    CHECK(rec.count(MaiEventType::PermissionAsked) == 0);
    CHECK(agent->listPendingPermissions().empty());
}

void test_interrupt_while_waiting_for_approval() {
    Workspace ws;
    FakeModel model;
    model.scripts = {
        Script{"", "write", R"({"path":"interrupted.txt","content":"x"})"},
        Script{"收到。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "写 interrupted.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    CHECK(agent->submit(MaiInterrupt{sid}).isOk());

    // 卡在等授权的那个线程必须醒过来，否则这一句永远返回不了。
    agent->waitIdle();

    CHECK(!ws.has("interrupted.txt"));
    CHECK(agent->listPendingPermissions().empty());
    CHECK(!agent->isBusy(sid));
}

void test_reply_to_stale_permission_is_not_found() {
    Workspace ws;
    FakeModel model;
    model.scripts = {
        Script{"", "write", R"({"path":"stale.txt","content":"x"})"},
        Script{"完事。", "", ""},
    };
    model.start();

    auto agent = makeAgent(model);
    const std::string sid = agent->submit(MaiCreateSession{ws.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sid, "写 stale.txt"});

    CHECK(waitFor([&] { return agent->listPendingPermissions().size() == 1; }));
    const std::string pid = agent->listPendingPermissions()[0].id;

    MaiReplyPermission reply;
    reply.permissionId = pid;
    reply.decision = MaiPermissionDecision::Once;
    CHECK(agent->submit(reply).isOk());
    agent->waitIdle();

    // 同一个 id 再点一次：界面重复点击、或者两个端同时点，都会走到这里。
    // 必须是明确的 NotFound，不能假装成功——界面要据此把对话框收掉。
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
    std::printf("-> test_read_never_asks\n");
    test_read_never_asks();
    std::printf("-> test_interrupt_while_waiting_for_approval\n");
    test_interrupt_while_waiting_for_approval();
    std::printf("-> test_reply_to_stale_permission_is_not_found\n");
    test_reply_to_stale_permission_is_not_found();

    if (failures) {
        std::printf("\n%d 项失败\n", failures);
        return 1;
    }
    std::printf("\npermission tests passed\n");
    return 0;
}
