#include "MaiSessionSchedule.h"

#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiTime.h"
#include "MaiScheduleTime.h"

namespace {

using Json = nlohmann::json;

Json serialize(const MaiScheduledAgentTask& task, bool includeInstruction) {
    Json value{{"task_id", task.id}, {"session_id", task.sessionId},
               {"title", task.title}, {"status", task.status},
               {"revision", task.revision},
               {"execute_at_utc", MaiTime::formatIso8601Utc(task.dueAtMs)},
               {"execute_at_utc_ms", task.dueAtMs}};
    if (includeInstruction) value["instruction"] = task.instruction;
    if (!task.messageId.empty()) value["message_id"] = task.messageId;
    if (!task.error.empty()) value["error"] = task.error;
    return value;
}

MaiToolResult invalid(const char* reason) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, reason);
}

class ScheduleTool final : public MaiTool {
public:
    enum class Action { Create, List, Update, Cancel };

    ScheduleTool(Action action, std::shared_ptr<MaiSessionScheduleStore> store,
                 std::string deviceName)
        : action_(action), store_(std::move(store)), deviceName_(std::move(deviceName)) {}

    std::string name() const override {
        switch (action_) {
            case Action::Create: return "maichat_session_task_create";
            case Action::List: return "maichat_session_task_list";
            case Action::Update: return "maichat_session_task_update";
            case Action::Cancel: return "maichat_session_task_cancel";
        }
        return {};
    }
    std::string description() const override {
        switch (action_) {
            case Action::Create:
                return "Schedule one future Agent instruction in the current MaiChat Session. "
                       "Call current_time first, resolve the user's time zone, and provide an "
                       "ISO 8601 timestamp with a UTC offset. Show title, full instruction, execution time and "
                       "owner device for approval. The owner App must stay running and the "
                       "device must be awake and online for on-time execution; after an outage "
                       "the task runs once within 24 hours. A mobile app suspended or "
                       "force-quit by the OS cannot run it on time. "
                       "If the user wants a system calendar reminder, call "
                       "maichat_calendar_create_event separately. Model and later tool "
                       "approvals still apply when the task runs.";
            case Action::List:
                return "List scheduled and recent completed Agent tasks in this MaiChat Session, "
                       "including execution device and status.";
            case Action::Update:
                return "Change the title, instruction or execution time of one not-yet-started "
                       "Agent task in this Session. Call maichat_session_task_list first and "
                       "use its task_id and revision. Omitted fields stay unchanged. Show the "
                       "complete resulting instruction and execution time for approval on every "
                       "call. An already started task cannot be changed. The owner App still "
                       "must be running, awake and online for on-time execution.";
            case Action::Cancel:
                return "Cancel a not-yet-started Agent task in this Session. Requires approval; "
                       "a started task must be stopped through the normal Session controls.";
        }
        return {};
    }
    std::string parametersSchema() const override {
        switch (action_) {
            case Action::Create:
                return R"({"type":"object","properties":{)"
                       R"("title":{"type":"string","minLength":1,"maxLength":120},)"
                       R"("instruction":{"type":"string","minLength":1,"maxLength":16384},)"
                       R"("execute_at":{"type":"string","minLength":20,"maxLength":25}},)"
                       R"("required":["title","instruction","execute_at"],)"
                       R"("additionalProperties":false})";
            case Action::List:
                return R"({"type":"object","properties":{)"
                       R"("offset":{"type":"integer","minimum":0,"maximum":199},)"
                       R"("limit":{"type":"integer","minimum":1,"maximum":50}},)"
                       R"("additionalProperties":false})";
            case Action::Update:
                return R"({"type":"object","properties":{)"
                       R"("task_id":{"type":"string"},)"
                       R"("expected_revision":{"type":"integer","minimum":1},)"
                       R"("title":{"type":"string","minLength":1,"maxLength":120},)"
                       R"("instruction":{"type":"string","minLength":1,"maxLength":16384},)"
                       R"("execute_at":{"type":"string","minLength":20,"maxLength":25}},)"
                       R"("required":["task_id","expected_revision"],)"
                       R"("additionalProperties":false})";
            case Action::Cancel:
                return R"({"type":"object","properties":{"task_id":{"type":"string"}},)"
                       R"("required":["task_id"],"additionalProperties":false})";
        }
        return {};
    }
    bool requiresApproval(const std::string&) const override {
        return action_ != Action::List;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return action_ != Action::List;
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (!store_) return MaiToolResult::failure(MaiErrorCode::NotConfigured,
            "scheduled Session tasks are not available on this device");
        if (context.isCanceled()) return MaiToolResult::failure(MaiErrorCode::Canceled,
            "scheduled task operation was canceled");
        if (context.sessionId.empty()) return invalid("a current Agent Session is required");
        const Json arguments = Json::parse(argumentsJson, nullptr, false);
        if (!arguments.is_object()) return invalid("arguments must be an object");
        if (action_ == Action::Create) {
            if (arguments.size() != 3 || !arguments.contains("title") ||
                !arguments["title"].is_string() || !arguments.contains("instruction") ||
                !arguments["instruction"].is_string() ||
                !arguments.contains("execute_at") ||
                !arguments["execute_at"].is_string())
                return invalid("title, instruction and an offset timestamp are required");
            const auto when = maiParseScheduledTime(
                arguments["execute_at"].get<std::string>());
            if (!when) return MaiToolResult::failure(when.error().code(),
                                                      when.error().message());
            const auto task = store_->create(context.sessionId,
                arguments["title"].get<std::string>(),
                arguments["instruction"].get<std::string>(),
                when.value(),
                MaiTime::getCurrentTime(),
                context.messageId.empty() || context.partId.empty() ? std::string{} :
                    context.messageId + ":" + context.partId);
            if (!task) return MaiToolResult::failure(task.error().code(),
                                                    task.error().message());
            Json output = serialize(task.value(), true);
            output["execution_device"] = deviceName_;
            output["catch_up_window_hours"] = 24;
            output["requires_owner_app_running"] = true;
            return MaiToolResult::success(output.dump());
        }
        if (action_ == Action::List) {
            if (arguments.size() > 2 ||
                (arguments.contains("offset") && !arguments["offset"].is_number_integer()) ||
                (arguments.contains("limit") && !arguments["limit"].is_number_integer()))
                return invalid("task list accepts only offset and limit");
            const int offset = arguments.value("offset", 0);
            const int limit = arguments.value("limit", 30);
            if (offset < 0 || offset > 199 || limit < 1 || limit > 50)
                return invalid("task list offset or limit is out of range");
            for (auto field = arguments.begin(); field != arguments.end(); ++field)
                if (field.key() != "offset" && field.key() != "limit")
                    return invalid("task list accepts only offset and limit");
            const auto listed = store_->list(context.sessionId);
            if (!listed) return MaiToolResult::failure(listed.error().code(),
                                                      listed.error().message());
            Json tasks = Json::array();
            const auto& all = listed.value();
            for (std::size_t index = std::size_t(offset);
                 index < all.size() && tasks.size() < std::size_t(limit); ++index)
                tasks.push_back(serialize(all[index], true));
            Json output{{"execution_device", deviceName_}, {"tasks", tasks},
                        {"total", all.size()}, {"offset", offset}};
            const std::size_t next = std::size_t(offset) + tasks.size();
            if (next < all.size()) output["next_offset"] = next;
            return MaiToolResult::success(output.dump(), next < all.size());
        }
        if (action_ == Action::Update) {
            if (arguments.size() < 3 || arguments.size() > 5 ||
                !arguments.value("task_id", Json{}).is_string() ||
                !arguments.value("expected_revision", Json{}).is_number_integer())
                return invalid("task_id, expected_revision and one changed field are required");
            for (auto field = arguments.begin(); field != arguments.end(); ++field)
                if (field.key() != "task_id" && field.key() != "expected_revision" &&
                    field.key() != "title" && field.key() != "instruction" &&
                    field.key() != "execute_at")
                    return invalid("unknown scheduled task update parameter");
            MaiScheduledAgentTaskUpdate changes;
            if (arguments.contains("title")) {
                if (!arguments["title"].is_string()) return invalid("title must be text");
                changes.title = arguments["title"].get<std::string>();
            }
            if (arguments.contains("instruction")) {
                if (!arguments["instruction"].is_string())
                    return invalid("instruction must be text");
                changes.instruction = arguments["instruction"].get<std::string>();
            }
            if (arguments.contains("execute_at")) {
                if (!arguments["execute_at"].is_string())
                    return invalid("execute_at must include a time zone offset");
                const auto when = maiParseScheduledTime(
                    arguments["execute_at"].get<std::string>());
                if (!when) return MaiToolResult::failure(when.error().code(),
                                                          when.error().message());
                changes.dueAtMs = when.value();
            }
            if (!changes.title && !changes.instruction && !changes.dueAtMs)
                return invalid("update needs a new title, instruction or execution time");
            const auto updated = store_->update(context.sessionId,
                arguments["task_id"].get<std::string>(), changes,
                MaiTime::getCurrentTime(),
                arguments["expected_revision"].get<std::int64_t>());
            if (!updated) return MaiToolResult::failure(updated.error().code(),
                                                        updated.error().message());
            Json output = serialize(updated.value(), true);
            output["execution_device"] = deviceName_;
            output["requires_owner_app_running"] = true;
            return MaiToolResult::success(output.dump());
        }
        if (arguments.size() != 1 || !arguments.contains("task_id") ||
            !arguments["task_id"].is_string())
            return invalid("task_id is required");
        const std::string taskId = arguments["task_id"].get<std::string>();
        const MaiError canceled = store_->cancel(context.sessionId, taskId);
        if (canceled.hasError())
            return MaiToolResult::failure(canceled.code(), canceled.message());
        return MaiToolResult::success(Json{{"task_id", taskId},
                                           {"status", "canceled"}}.dump());
    }

private:
    Action action_;
    std::shared_ptr<MaiSessionScheduleStore> store_;
    std::string deviceName_;
};

}  // namespace

void registerMaiSessionScheduleTools(MaiToolRegistry& tools,
    std::shared_ptr<MaiSessionScheduleStore> store,
    const std::string& deviceName) {
    for (const auto action : {ScheduleTool::Action::Create,
                              ScheduleTool::Action::List,
                              ScheduleTool::Action::Update,
                              ScheduleTool::Action::Cancel})
        tools.add(std::make_unique<ScheduleTool>(action, store, deviceName));
}
