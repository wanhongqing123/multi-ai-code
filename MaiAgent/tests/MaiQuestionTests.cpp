// 问答闸门和 question 工具的测试。
//
// 重点是**没等到答案的那几种情况**。编一个答案回给模型，它会基于一个用户
// 从没说过的决定往下做，而且完全不知道那是编的——这比直接失败糟得多。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiIdGenerator.h"
#include "MaiQuestion.h"
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

MaiQuestionRequest makeRequest(const std::string& text) {
    MaiQuestionRequest request;
    request.id = MaiIdGenerator::newQuestionId();
    request.sessionId = "ses_q";
    request.messageId = "msg_q";
    request.partId = "prt_q";
    request.question = text;
    return request;
}

// ── 闸门 ────────────────────────────────────────────────────────

void test_ask_blocks_until_someone_answers() {
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    const MaiQuestionRequest request = makeRequest("which one?");

    std::string announced;
    std::thread replier([&gate, &request] {
        // 等 ask() 登记好。登记之前回答的话 reply 找不到那个 id——
        // 闸门内部先登记再广播就是为了挡住这种竞态，这里模拟的是慢一点的界面。
        for (int i = 0; i < 200; ++i) {
            if (gate.reply(request.id, "the second one")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    const std::string answer = gate.ask(
        request, [&announced](const MaiQuestionRequest& r) { announced = r.question; }, cancel);
    replier.join();

    CHECK(answer == "the second one");
    // 广播必须发生：不发的话界面根本不知道有人在等，这一轮会一直挂着。
    CHECK(announced == "which one?");
    // 结束之后不该还挂在待回答列表里。
    CHECK(gate.listPending().empty());
}

void test_interrupting_gives_no_answer() {
    // **不能编一个答案。** 被中断就是被中断。
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true);
    });

    const auto began = std::chrono::steady_clock::now();
    const std::string answer = gate.ask(makeRequest("anything?"), nullptr, cancel);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    stopper.join();

    CHECK(answer.empty());
    CHECK(took < 5000);
}

void test_timeout_gives_no_answer() {
    MaiQuestionGate::Options options;
    options.timeoutMs = 300;
    MaiQuestionGate gate(options);
    std::atomic<bool> cancel{false};

    const std::string answer = gate.ask(makeRequest("anything?"), nullptr, cancel);
    CHECK(answer.empty());
}

void test_an_empty_answer_is_still_an_answer() {
    // 用户可能就是想说「你看着办」。但那和「没人回答」是两回事，
    // 不能撞成同一个空串——撞上的话模型会把「你看着办」当成「被中断了」。
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    const MaiQuestionRequest request = makeRequest("which one?");

    std::thread replier([&gate, &request] {
        for (int i = 0; i < 200; ++i) {
            if (gate.reply(request.id, "")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const std::string answer = gate.ask(request, nullptr, cancel);
    replier.join();

    CHECK(!answer.empty());
    CHECK(answer.find("nothing") != std::string::npos);
}

void test_replying_to_an_unknown_question_is_not_a_crash() {
    // 界面重复点、或者对着已经被中断的提问回答，都会走到这里。不是故障，
    // 但也不能假装成功——界面要据此把那个过期的输入框收掉。
    MaiQuestionGate gate;
    CHECK(!gate.reply("qst_nope", "hello"));
}

void test_pending_questions_are_listed_while_waiting() {
    // 界面刷新后要能重新看到还在等什么。
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    const MaiQuestionRequest request = makeRequest("still waiting?");

    std::thread asker([&gate, &request, &cancel] { gate.ask(request, nullptr, cancel); });
    bool sawPending = false;
    for (int i = 0; i < 200 && !sawPending; ++i) {
        const auto pending = gate.listPending();
        sawPending = pending.size() == 1 && pending[0].question == "still waiting?";
        if (!sawPending) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    cancel.store(true);
    asker.join();
    CHECK(sawPending);
}

// ── 工具 ────────────────────────────────────────────────────────

void test_without_a_gate_the_tool_says_so_instead_of_hanging() {
    // 宿主没接问答时干等的话，用户看到的是「卡住了」，模型收到的是「超时」，
    // 两边都不知道真相。
    auto tool = makeMaiQuestionTool();
    MaiToolContext context;
    context.sessionId = "ses_q";
    const MaiToolResult result =
        tool->execute(args({{"question", "which one?"}}), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotSupported);
    // 要告诉模型下一步能怎么办，而不是只说不行。
    CHECK(result.error().message().find("Decide for yourself") != std::string::npos);
}

void test_the_tool_returns_the_answer_verbatim() {
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    auto tool = makeMaiQuestionTool();

    MaiToolContext context;
    context.sessionId = "ses_q";
    context.messageId = "msg_q";
    context.partId = "prt_q";
    context.cancel = &cancel;
    context.questions = &gate;
    context.announceQuestion = [](const MaiQuestionRequest&) {};

    std::thread replier([&gate] {
        for (int i = 0; i < 400; ++i) {
            const auto pending = gate.listPending();
            if (!pending.empty() && gate.reply(pending[0].id, "use the second design")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const MaiToolResult result = tool->execute(
        args({{"question", "which design?"}, {"options", {"first", "second"}}}), context);
    replier.join();

    CHECK(!result.hasError());
    CHECK(result.output() == "use the second design");
}

void test_the_tool_reports_an_interrupt_as_an_interrupt() {
    // **不能说成「用户拒绝了」** ——用户可能根本没看见这次提问。
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    auto tool = makeMaiQuestionTool();

    MaiToolContext context;
    context.sessionId = "ses_q";
    context.cancel = &cancel;
    context.questions = &gate;
    context.announceQuestion = [](const MaiQuestionRequest&) {};

    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true);
    });
    const MaiToolResult result = tool->execute(args({{"question", "anything?"}}), context);
    stopper.join();

    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::Canceled);
    CHECK(result.error().message().find("did not answer") != std::string::npos);
}

void test_the_tool_carries_the_options_and_ids_into_the_request() {
    // 界面靠 partId 认出是哪一次调用在等回答；选项给了的话可以摆成按钮。
    MaiQuestionGate gate;
    std::atomic<bool> cancel{false};
    auto tool = makeMaiQuestionTool();

    MaiToolContext context;
    context.sessionId = "ses_carry";
    context.messageId = "msg_carry";
    context.partId = "prt_carry";
    context.cancel = &cancel;
    context.questions = &gate;

    MaiQuestionRequest seen;
    context.announceQuestion = [&seen](const MaiQuestionRequest& r) { seen = r; };

    std::thread replier([&gate] {
        for (int i = 0; i < 400; ++i) {
            const auto pending = gate.listPending();
            if (!pending.empty() && gate.reply(pending[0].id, "a")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    tool->execute(args({{"question", "pick"}, {"options", {"a", "b", "c"}}}), context);
    replier.join();

    CHECK(seen.sessionId == "ses_carry");
    CHECK(seen.messageId == "msg_carry");
    CHECK(seen.partId == "prt_carry");
    CHECK(seen.options.size() == 3);
    CHECK(seen.options[1] == "b");
    CHECK(seen.id.compare(0, 4, "qst_") == 0);
}

void test_question_needs_no_approval_and_is_registered() {
    // 它不改任何东西，而且自己就是在等用户说话。再套一层「要不要允许提问」
    // 只会多一次点击。
    auto tool = makeMaiQuestionTool();
    CHECK(!tool->requiresApproval(args({{"question", "x"}})));
    CHECK(tool->execute("{}", MaiToolContext{}).hasError());

    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK(registry.find("question") != nullptr);
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_ask_blocks_until_someone_answers);
    RUN(test_interrupting_gives_no_answer);
    RUN(test_timeout_gives_no_answer);
    RUN(test_an_empty_answer_is_still_an_answer);
    RUN(test_replying_to_an_unknown_question_is_not_a_crash);
    RUN(test_pending_questions_are_listed_while_waiting);
    RUN(test_without_a_gate_the_tool_says_so_instead_of_hanging);
    RUN(test_the_tool_returns_the_answer_verbatim);
    RUN(test_the_tool_reports_an_interrupt_as_an_interrupt);
    RUN(test_the_tool_carries_the_options_and_ids_into_the_request);
    RUN(test_question_needs_no_approval_and_is_registered);
    if (failures == 0) std::printf("question tests passed\n");
    return failures == 0 ? 0 : 1;
}
