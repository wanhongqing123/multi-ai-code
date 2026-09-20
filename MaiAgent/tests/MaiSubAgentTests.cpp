// 子 Agent 的测试。
//
// 重点是**三条硬规矩**，不是「能不能跑起来」：
//
//   深度上限   不封的话模型能把自己 fork 到爆，每一层都觉得这活该交出去
//   数量上限   同上，而且每个都在烧钱
//   横向隔离   拿别人的会话 id 去派活 / 去等 / 去收，一律要挡住
//
// 前两条判错了是烧钱，第三条判错了是越权。所以这几条的用例比"跑通"多。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiFakeModelClient.h"
#include "MaiSessionStore.h"
#include "MaiTool.h"

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

std::string args(const json& j) {
    return j.dump();
}

// 一个只会说一句 "done" 就收工的模型。
//
// 用 setRepeatingTurn 而不是按序号取剧本：这些用例会同时起好几个子 Agent，
// 按序号取的话它们各拿到剧本里不同的一条，而这里要的是每个都一样地跑完。
std::unique_ptr<MaiAgent> makeAgent(MaiAgent::Options options = {}) {
    auto model = std::make_unique<MaiFakeModelClient>();
    MaiFakeModelClient::Turn turn;
    turn.textChunks = {"done"};
    model->setRepeatingTurn(turn);
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);
    return std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), std::move(tools),
                                      options);
}

std::string newRootSession(MaiAgent& agent) {
    MaiResult<std::string> created = agent.submit(MaiCreateSession{"", "."});
    return created ? created.value() : std::string();
}

// ── 深度上限 ────────────────────────────────────────────────────

void test_depth_is_capped() {
    MaiAgent::Options options;
    options.maxSubAgentDepth = 2;
    auto agent = makeAgent(options);
    const std::string root = newRootSession(*agent);

    MaiResult<std::string> first = agent->spawnSubAgent(root, "one", "do a thing");
    CHECK(first.isOk());
    MaiResult<std::string> second = agent->spawnSubAgent(first.value(), "two", "do a thing");
    CHECK(second.isOk());

    // 第三层到顶。**报错要说清是深度到顶**，模型才知道是「别再往下分」
    // 而不是「先收掉几个」。
    MaiResult<std::string> third = agent->spawnSubAgent(second.value(), "three", "do a thing");
    CHECK(!third.isOk());
    CHECK(third.error().message().find("deep") != std::string::npos);

    agent->waitIdle();
}

void test_depth_is_recorded_on_the_session() {
    auto agent = makeAgent();
    const std::string root = newRootSession(*agent);
    MaiResult<std::string> child = agent->spawnSubAgent(root, "one", "do a thing");
    CHECK(child.isOk());

    MaiSession stored;
    CHECK(agent->getSession(child.value(), stored));
    CHECK(stored.parentId == root);
    CHECK(stored.depth == 1);
    CHECK(!stored.isRoot());

    MaiSession rootSession;
    CHECK(agent->getSession(root, rootSession));
    CHECK(rootSession.isRoot());
    CHECK(rootSession.depth == 0);

    agent->waitIdle();
}

// ── 数量上限 ────────────────────────────────────────────────────

void test_open_count_is_capped_and_closing_frees_a_slot() {
    MaiAgent::Options options;
    options.maxOpenSubAgents = 2;
    auto agent = makeAgent(options);
    const std::string root = newRootSession(*agent);

    MaiResult<std::string> a = agent->spawnSubAgent(root, "a", "x");
    MaiResult<std::string> b = agent->spawnSubAgent(root, "b", "x");
    CHECK(a.isOk());
    CHECK(b.isOk());

    MaiResult<std::string> c = agent->spawnSubAgent(root, "c", "x");
    CHECK(!c.isOk());
    // 这条报的是数量，不是深度——两种的应对完全不一样。
    CHECK(c.error().message().find("open") != std::string::npos);

    // 收掉一个就该腾出名额。
    CHECK(!agent->closeSubAgent(root, a.value()).hasError());
    MaiResult<std::string> d = agent->spawnSubAgent(root, "d", "x");
    CHECK(d.isOk());

    agent->waitIdle();
}

void test_closing_keeps_the_messages() {
    // 收掉只是腾名额。消息删了的话，用户排障时唯一的线索就没了。
    auto agent = makeAgent();
    const std::string root = newRootSession(*agent);
    MaiResult<std::string> child = agent->spawnSubAgent(root, "a", "remember this");
    CHECK(child.isOk());
    agent->waitIdle();

    const std::size_t before = agent->listMessages(child.value()).size();
    CHECK(before > 0);
    CHECK(!agent->closeSubAgent(root, child.value()).hasError());
    CHECK(agent->listMessages(child.value()).size() == before);
}

// ── 横向隔离 ────────────────────────────────────────────────────

void test_a_session_cannot_touch_someone_elses_child() {
    // **判错这条是越权**：任何一个会话都能拿到别人的会话 id（它们就是字符串），
    // 不查父子关系的话就能给别人派活、等别人、收掉别人。
    auto agent = makeAgent();
    const std::string mine = newRootSession(*agent);
    const std::string theirs = newRootSession(*agent);

    MaiResult<std::string> theirChild = agent->spawnSubAgent(theirs, "theirs", "x");
    CHECK(theirChild.isOk());
    agent->waitIdle();

    // 派活
    const MaiError sent = agent->sendToSubAgent(mine, theirChild.value(), "do my bidding");
    CHECK(sent.hasError());
    // 收掉
    const MaiError closed = agent->closeSubAgent(mine, theirChild.value());
    CHECK(closed.hasError());
    // 等
    std::atomic<bool> cancel{false};
    CHECK(!agent->waitForSubAgent(mine, theirChild.value(), 200, cancel));

    // 它也不该出现在我的清单里。
    CHECK(agent->listSubAgents(mine).empty());
    CHECK(agent->listSubAgents(theirs).size() == 1);

    // **一定要排空。** 越权检查一旦失效，上面那次 sendToSubAgent 会真的起一轮，
    // 不排空的话进程会挂在析构里 join 一个还在跑的线程上——表现成「测试卡住」，
    // 而不是「断言红了」，那就看不出是哪条规矩破了。
    agent->waitIdle();
}

void test_the_child_inherits_the_parents_directory() {
    // 「子只能比父弱」落到实处的第一条：子看得见的文件是父的子集。
    // 有参数能改目录的话，子就能跑到父看不见的地方去。
    auto agent = makeAgent();
    MaiResult<std::string> created = agent->submit(MaiCreateSession{"", "."});
    CHECK(created.isOk());
    MaiSession parent;
    CHECK(agent->getSession(created.value(), parent));

    MaiResult<std::string> child = agent->spawnSubAgent(created.value(), "a", "x");
    CHECK(child.isOk());
    MaiSession childSession;
    CHECK(agent->getSession(child.value(), childSession));
    CHECK(childSession.directory == parent.directory);

    agent->waitIdle();
}

// ── 会话列表 ────────────────────────────────────────────────────

void test_sub_agents_do_not_show_up_in_the_session_list() {
    // 用户没开过它们。列出来只会让人以为自己漏了什么。
    auto agent = makeAgent();
    const std::string root = newRootSession(*agent);
    CHECK(agent->spawnSubAgent(root, "a", "x").isOk());
    CHECK(agent->spawnSubAgent(root, "b", "x").isOk());
    agent->waitIdle();

    CHECK(agent->listSessions().size() == 1);
    CHECK(agent->listSessions()[0].id == root);
    // 但对父自己是看得见的。
    CHECK(agent->listSubAgents(root).size() == 2);
}

void test_status_reflects_running_idle_and_closed() {
    auto agent = makeAgent();
    const std::string root = newRootSession(*agent);
    MaiResult<std::string> child = agent->spawnSubAgent(root, "a", "x");
    CHECK(child.isOk());
    agent->waitIdle();

    auto children = agent->listSubAgents(root);
    CHECK(children.size() == 1);
    CHECK(children[0].status == "idle");
    CHECK(children[0].taskName == "a");
    CHECK(children[0].depth == 1);

    CHECK(!agent->closeSubAgent(root, child.value()).hasError());
    children = agent->listSubAgents(root);
    CHECK(children.size() == 1);
    CHECK(children[0].status == "closed");
}

void test_waiting_returns_what_the_child_said() {
    auto agent = makeAgent();
    const std::string root = newRootSession(*agent);
    MaiResult<std::string> child = agent->spawnSubAgent(root, "a", "x");
    CHECK(child.isOk());

    std::atomic<bool> cancel{false};
    CHECK(agent->waitForSubAgent(root, child.value(), 10000, cancel));
    CHECK(agent->subAgentReport(child.value()) == "done");
}

// ── 工具层 ──────────────────────────────────────────────────────

void test_tools_say_so_when_there_is_no_host() {
    // 宿主没接子 Agent 时失败得莫名其妙的话，模型会反复试。
    MaiToolContext context;
    context.sessionId = "ses_x";
    for (auto& tool : {makeMaiSpawnAgentTool(), makeMaiWaitAgentTool(), makeMaiSendInputTool(),
                       makeMaiListAgentsTool(), makeMaiCloseAgentTool()}) {
        const MaiToolResult result = tool->execute("{}", context);
        CHECK(result.hasError());
        CHECK(result.error().code() == MaiErrorCode::NotSupported);
    }
}

void test_all_five_tools_are_registered_and_read_only_ones_need_no_approval() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    for (const char* name :
         {"spawn_agent", "wait_agent", "send_input", "list_agents", "close_agent"}) {
        CHECK(registry.find(name) != nullptr);
    }
    // 起子 Agent 不单独要审批：它能干的事早就被工作目录和它自己的授权闸挡着了，
    // 再加一道只是多一次点击。
    CHECK(!registry.find("spawn_agent")->requiresApproval("{}"));
    CHECK(!registry.find("list_agents")->requiresApproval("{}"));
}

void test_waiting_on_a_still_running_child_is_not_an_error() {
    // **说成失败的话模型会去收拾一个根本没出错的子 Agent。**
    auto agent = makeAgent();
    const std::string root = newRootSession(*agent);
    auto tool = makeMaiWaitAgentTool();

    MaiToolContext context;
    context.sessionId = root;
    context.subAgents = agent.get();

    // 一个不存在的 id：等不到，但也不该当成工具出错。
    const MaiToolResult result =
        tool->execute(args({{"agent_id", "ses_nope"}, {"timeout_ms", 1000}}), context);
    CHECK(!result.hasError());
    CHECK(result.output().find("still working") != std::string::npos);
    agent->waitIdle();
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_depth_is_capped);
    RUN(test_depth_is_recorded_on_the_session);
    RUN(test_open_count_is_capped_and_closing_frees_a_slot);
    RUN(test_closing_keeps_the_messages);
    RUN(test_a_session_cannot_touch_someone_elses_child);
    RUN(test_the_child_inherits_the_parents_directory);
    RUN(test_sub_agents_do_not_show_up_in_the_session_list);
    RUN(test_status_reflects_running_idle_and_closed);
    RUN(test_waiting_returns_what_the_child_said);
    RUN(test_tools_say_so_when_there_is_no_host);
    RUN(test_all_five_tools_are_registered_and_read_only_ones_need_no_approval);
    RUN(test_waiting_on_a_still_running_child_is_not_an_error);
    if (failures == 0) std::printf("sub-agent tests passed\n");
    return failures == 0 ? 0 : 1;
}
