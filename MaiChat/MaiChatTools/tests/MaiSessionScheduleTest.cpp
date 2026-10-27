#include "MaiSessionSchedule.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <json.hpp>
#include <sqlite3.h>

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
    const auto editableStore = MaiSessionScheduleStore::open(":memory:");
    require(editableStore.isOk(), "cannot open editable schedule store");
    const std::int64_t now = 1'800'000'000'000LL;
    const auto editable = editableStore.value()->create(
        "ses_edit", "旧标题", "旧指令", now + 60'000, now);
    require(editable.isOk() && editable.value().revision == 1,
            "new task has an invalid revision");
    MaiScheduledAgentTaskUpdate changes;
    changes.title = "新标题";
    changes.dueAtMs = now + 120'000;
    const auto edited = editableStore.value()->update(
        "ses_edit", editable.value().id, changes, now, editable.value().revision);
    require(edited.isOk() && edited.value().revision == 2 &&
            edited.value().instruction == "旧指令" && edited.value().dueAtMs == now + 120'000,
            "partial scheduled task update lost unchanged fields");
    require(editableStore.value()->update("ses_edit", editable.value().id,
        changes, now, 1).error().code() == MaiErrorCode::Busy,
        "stale task revision overwrote a newer edit");
    require(!editableStore.value()->update("ses_other", editable.value().id,
        changes, now, 2).isOk(),
        "another Session edited the task");
    require(editableStore.value()->claimDue(now + 60'000).value().empty(),
            "task still ran at its old execution time");
    const auto claimedEdit = editableStore.value()->claimDue(now + 120'000);
    require(claimedEdit.isOk() && claimedEdit.value().size() == 1 &&
            claimedEdit.value()[0].title == "新标题",
            "edited task did not run once at the new time");
    require(!editableStore.value()->update("ses_edit", editable.value().id,
        changes, now, 2).isOk(),
        "started task was edited");

    const std::string legacyPath = root.append(
        MaiFilePath::fromUtf8("legacy.sqlite")).toUtf8();
    sqlite3* legacy = nullptr;
    require(sqlite3_open(legacyPath.c_str(), &legacy) == SQLITE_OK,
            "cannot create legacy schedule database");
    const char* legacySql =
        "CREATE TABLE maichat_scheduled_tasks("
        "id TEXT PRIMARY KEY,session_id TEXT NOT NULL,title TEXT NOT NULL,"
        "instruction TEXT NOT NULL,due_at_ms INTEGER NOT NULL,"
        "created_at_ms INTEGER NOT NULL,started_at_ms INTEGER NOT NULL DEFAULT 0,"
        "status TEXT NOT NULL,message_id TEXT NOT NULL DEFAULT '',"
        "error TEXT NOT NULL DEFAULT '',origin_key TEXT);"
        "INSERT INTO maichat_scheduled_tasks"
        "(id,session_id,title,instruction,due_at_ms,created_at_ms,status) VALUES"
        "('schtask_legacy','ses_legacy','old','prompt',1800000060000,1800000000000,'scheduled');";
    require(sqlite3_exec(legacy, legacySql, nullptr, nullptr, nullptr) == SQLITE_OK,
            "cannot seed legacy schedule database");
    sqlite3_close(legacy);
    const auto migrated = MaiSessionScheduleStore::open(legacyPath);
    require(migrated.isOk() && migrated.value()->list("ses_legacy").value()[0].revision == 1,
            "old scheduled task database did not gain revisions");

    const std::string path = root.append(MaiFilePath::fromUtf8("tasks.sqlite")).toUtf8();
    const auto opened = MaiSessionScheduleStore::open(path);
    require(opened.isOk(), "cannot open schedule store");
    const auto store = opened.value();
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
    require(tools.specs().size() == 4, "schedule tool registry is incomplete");
    MaiToolContext context;
    context.sessionId = "ses_owner";
    auto* create = tools.findForModel("maichat_session_task_create");
    auto* list = tools.findForModel("maichat_session_task_list");
    auto* update = tools.findForModel("maichat_session_task_update");
    auto* cancel = tools.findForModel("maichat_session_task_cancel");
    for (MaiTool* tool : {create, list, update, cancel})
        require(tool && nlohmann::json::parse(tool->parametersSchema()).is_object(),
                "scheduled task schema is invalid");
    require(create && list && update && cancel && create->requiresPerCallApproval("{}") &&
            !list->requiresApproval("{}") && update->requiresPerCallApproval("{}") &&
            cancel->requiresPerCallApproval("{}"),
            "schedule approval contract is wrong");
    for (int index = 0; index < 35; ++index)
        require(store->create("ses_page", "task " + std::to_string(index), "check",
            now + 120'000 + index * 1000, now).isOk(),
            "could not seed paged task list");
    MaiToolContext pageContext;
    pageContext.sessionId = "ses_page";
    const auto page = list->execute(R"({"offset":30,"limit":10})", pageContext);
    require(!page.hasError() &&
            nlohmann::json::parse(page.output())["tasks"].size() == 5 &&
            nlohmann::json::parse(page.output())["tasks"][4]["title"] == "task 0",
            "scheduled task beyond the first page cannot be selected for an edit");
    const auto scheduled = create->execute(nlohmann::json{
        {"title", "明日检查"}, {"instruction", "检查 Agent 输出"},
        {"execute_at", MaiTime::formatIso8601Utc(MaiTime::getCurrentTime() + 60'000)}
    }.dump(), context);
    require(!scheduled.hasError(), "Agent tool could not schedule a real-time task");
    const auto created = nlohmann::json::parse(scheduled.output());
    const auto changed = update->execute(nlohmann::json{
        {"task_id", created["task_id"]}, {"expected_revision", created["revision"]},
        {"execute_at", MaiTime::formatIso8601Utc(MaiTime::getCurrentTime() + 180'000)}
    }.dump(), context);
    require(!changed.hasError() && nlohmann::json::parse(changed.output())["revision"] == 2,
            "Agent tool did not update a scheduled task");
    require(update->execute(nlohmann::json{
        {"task_id", created["task_id"]}, {"expected_revision", created["revision"]},
        {"title", "过期修改"}
    }.dump(), context).hasError(),
            "Agent tool accepted a stale scheduled task revision");
    require(!list->execute("{}", context).hasError(),
            "Agent tool could not list Session tasks");
    context.sessionId = "ses_other";
    require(update->execute(nlohmann::json{
        {"task_id", created["task_id"]}, {"expected_revision", 2},
        {"title", "跨会话修改"}
    }.dump(), context).hasError(),
            "another Session used the Agent tool to edit a task");
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
