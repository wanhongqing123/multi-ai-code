#include "MaiSessionSchedule.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiTime.h"
#include "MaiScheduleTime.h"

namespace {

void require(bool value, const char* reason) {
    if (!value) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto offset = maiParseScheduledTime("2026-10-28T09:00:00+08:00");
    const auto utc = maiParseScheduledTime("2026-10-28T01:00:00Z");
    require(offset.isOk() && utc.isOk() && offset.value() == utc.value(),
            "schedule parser changed an explicit time zone offset");
    require(!maiParseScheduledTime("2026-02-30T09:00:00+08:00").isOk() &&
            !maiParseScheduledTime("2026-10-28T09:00:00").isOk(),
            "ambiguous or invalid scheduled date was accepted");
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("schedule_test_")));
    {
    require(!MaiFileSystem::createDirectories(root).hasError(),
            "cannot make schedule test directory");
    const std::string path = root.append(MaiFilePath::fromUtf8("tasks.sqlite")).toUtf8();
    const auto opened = MaiSessionScheduleStore::open(path);
    require(opened.isOk(), "cannot open schedule store");
    const auto store = opened.value();
    const std::int64_t now = 1'800'000'000'000LL;
    const auto task = store->create("ses_owner", "检查构建", "检查测试并总结", now + 60'000, now);
    require(task.isOk() && task.value().status == "scheduled", "could not schedule task");
    const auto idempotent = store->create("ses_idempotent", "同一请求", "只执行一次",
        now + 300'000, now, "message:part");
    const auto retried = store->create("ses_idempotent", "同一请求", "只执行一次",
        now + 300'000, now + 310'000, "message:part");
    require(idempotent.isOk() && retried.isOk() &&
            idempotent.value().id == retried.value().id,
            "retried Agent tool call created a duplicate scheduled task");
    require(!store->create("ses_idempotent", "同一请求", "不同内容",
        now + 300'000, now, "message:part").isOk(),
        "one task request ID accepted different content");
    require(!store->cancel("ses_idempotent", idempotent.value().id).hasError(),
            "idempotent test task could not be removed");
    require(!store->create("ses_owner", "bad", "prompt", now - 1, now).isOk(),
            "past task was scheduled");
    require(store->list("ses_other").value().empty(),
            "task leaked to another Session");
    require(store->claimDue(now + 59'000).value().empty(),
            "task ran before its due time");
    const auto second = MaiSessionScheduleStore::open(path);
    require(second.isOk(), "cannot open concurrent schedule store");
    std::optional<MaiResult<std::vector<MaiScheduledAgentTask>>> firstResult;
    std::optional<MaiResult<std::vector<MaiScheduledAgentTask>>> secondResult;
    std::thread firstClaim([&] { firstResult.emplace(store->claimDue(now + 60'000)); });
    std::thread secondClaim([&] {
        secondResult.emplace(second.value()->claimDue(now + 60'000));
    });
    firstClaim.join();
    secondClaim.join();
    require(firstResult.has_value() && secondResult.has_value(),
            "concurrent claims did not finish");
    const auto& first = firstResult.value();
    const auto& competing = secondResult.value();
    require(first.isOk() && competing.isOk() &&
            first.value().size() + competing.value().size() == 1,
            "two owner runners claimed the same task");
    require(store->cancel("ses_owner", task.value().id).hasError(),
            "started task was canceled");
    require(!store->markSubmitted(task.value().id, "msg_one").hasError(),
            "submitted message ID was not saved");
    require(!store->markFinished(task.value().id, true).hasError(),
            "completed status was not saved");
    require(store->list("ses_owner").value()[0].status == "completed",
            "completed task is missing from Session");

    const auto busy = store->create("ses_owner", "忙碌", "稍后执行", now + 70'000, now);
    require(busy.isOk(), "busy task was not saved");
    require(store->claimDue(now + 70'000).value().size() == 1,
            "busy task was not claimed");
    require(!store->releaseBusy(busy.value().id).hasError() &&
            store->claimDue(now + 80'000).value().size() == 1,
            "busy Session could not retry its unsubmitted task");
    require(!store->markInterruptedUncertain().hasError() &&
            store->list("ses_owner").value()[0].status == "uncertain",
            "interrupted task could be submitted twice");

    const auto expired = store->create("ses_owner", "过期", "不要执行", now + 100'000, now);
    require(expired.isOk(), "expired fixture was not created");
    require(store->claimDue(now + 100'000 + 24LL * 60 * 60 * 1000 + 1).value().empty(),
            "task beyond its catch-up window ran");
    require(store->list("ses_owner").value()[0].status == "missed",
            "old task was not marked missed");

    MaiToolRegistry tools;
    registerMaiSessionScheduleTools(tools, store, "Mac");
    require(tools.specs().size() == 3, "schedule tool registry is incomplete");
    MaiToolContext context;
    context.sessionId = "ses_owner";
    auto* create = tools.findForModel("maichat_session_task_create");
    auto* list = tools.findForModel("maichat_session_task_list");
    auto* cancel = tools.findForModel("maichat_session_task_cancel");
    require(create && list && cancel && create->requiresPerCallApproval("{}") &&
            !list->requiresApproval("{}") && cancel->requiresPerCallApproval("{}"),
            "schedule approval contract is wrong");
    const auto scheduled = create->execute(nlohmann::json{
        {"title", "明日检查"}, {"instruction", "检查 Agent 输出"},
        {"execute_at", MaiTime::formatIso8601Utc(MaiTime::getCurrentTime() + 60'000)}
    }.dump(), context);
    require(!scheduled.hasError(), "Agent tool could not schedule a real-time task");
    const auto created = nlohmann::json::parse(scheduled.output());
    require(!list->execute("{}", context).hasError(),
            "Agent tool could not list Session tasks");
    context.sessionId = "ses_other";
    require(cancel->execute(nlohmann::json{{"task_id", created["task_id"]}}.dump(),
                            context).hasError(),
            "another Session canceled the task");
    context.sessionId = "ses_owner";
    require(!cancel->execute(nlohmann::json{{"task_id", created["task_id"]}}.dump(),
                             context).hasError(),
            "Agent tool could not cancel its Session task");

    const auto runnerStore = MaiSessionScheduleStore::open(":memory:");
    require(runnerStore.isOk(), "cannot open scheduler test store");
    const auto due = runnerStore.value()->create("ses_runner", "到点", "执行一次",
                                                 now + 5'000, now);
    require(due.isOk(), "runner task was not saved");
    int submitted = 0;
    bool runnerBusy = true;
    MaiSessionScheduleRunner runner(runnerStore.value(),
        [](const std::string& session) { return session == "ses_runner"; },
        [&](const std::string&) { return runnerBusy; },
        [&](const std::string& session, const std::string& instruction)
            -> MaiResult<std::string> {
            require(session == "ses_runner" && instruction == "执行一次",
                    "runner routed the wrong Session instruction");
            ++submitted;
            return std::string("msg_scheduled");
        });
    require(!runner.poll(now + 5'000).hasError() && submitted == 0,
            "runner submitted while Session was busy");
    runnerBusy = false;
    require(!runner.poll(now + 6'000).hasError() && submitted == 1,
            "runner did not submit the due instruction once");
    require(!runner.poll(now + 7'000).hasError() && submitted == 1,
            "runner submitted the same instruction twice");
    require(!runner.finish("ses_runner").hasError() &&
            runnerStore.value()->list("ses_runner").value()[0].status == "completed",
            "runner did not record the completed turn");
    }
    MaiFileSystem::removeRecursively(root);
}
