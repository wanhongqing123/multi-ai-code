#include "MaiCalendarTools.h"

#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiTime.h"
#include "MaiScheduleTime.h"

MaiError maiValidateCalendarEvent(const MaiCalendarEventInput& event,
                                  std::int64_t nowMs) {
    if (event.title.empty() || event.title.size() > 240 ||
        event.title.find('\0') != std::string::npos ||
        event.notes.size() > 4000 || event.notes.find('\0') != std::string::npos ||
        event.sessionId.empty() || event.sessionId.size() > 128 ||
        nowMs <= 0 || event.startsAtUtcMs <= nowMs ||
        event.endsAtUtcMs <= event.startsAtUtcMs ||
        event.endsAtUtcMs - event.startsAtUtcMs > 7LL * 24 * 60 * 60 * 1000 ||
        event.startsAtUtcMs - nowMs > 366LL * 24 * 60 * 60 * 1000)
        return {MaiErrorCode::InvalidInput,
                "calendar event needs a future start, later end, title and current Session"};
    return {};
}

namespace {

using Json = nlohmann::json;

class CalendarCreateTool final : public MaiTool {
public:
    explicit CalendarCreateTool(MaiCalendarCreateHandler create)
        : create_(std::move(create)) {}

    std::string name() const override { return "maichat_calendar_create_event"; }
    std::string description() const override {
        return "Add one event to the device's system calendar, or open an import "
               "sheet where direct writing is unavailable. Call current_time first "
               "and confirm the user's time zone. When approval is requested, show the "
               "full title, times and notes. A calendar event reminds the user; it does not run an "
               "Agent task. Use maichat_session_task_create separately for execution.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"title":{"type":"string","minLength":1,"maxLength":120},"notes":{"type":"string","maxLength":4000},"starts_at":{"type":"string","minLength":20,"maxLength":25},"ends_at":{"type":"string","minLength":20,"maxLength":25}},"required":["title","starts_at","ends_at"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return true; }
    bool requiresPerCallApproval(const std::string&) const override { return true; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "calendar event creation was canceled");
        if (!create_)
            return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                          "system calendar is unavailable on this device");
        const Json arguments = Json::parse(argumentsJson, nullptr, false);
        if (!arguments.is_object() || arguments.size() < 3 || arguments.size() > 4 ||
            !arguments.contains("title") || !arguments["title"].is_string() ||
            !arguments.contains("starts_at") ||
            !arguments["starts_at"].is_string() ||
            !arguments.contains("ends_at") ||
            !arguments["ends_at"].is_string() ||
            (arguments.contains("notes") && !arguments["notes"].is_string()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "title, start and end offset timestamps are required");
        for (auto it = arguments.begin(); it != arguments.end(); ++it)
            if (it.key() != "title" && it.key() != "notes" &&
                it.key() != "starts_at" && it.key() != "ends_at")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unknown calendar event parameter");
        const auto start = maiParseScheduledTime(
            arguments["starts_at"].get<std::string>());
        const auto end = maiParseScheduledTime(
            arguments["ends_at"].get<std::string>());
        if (!start) return MaiToolResult::failure(start.error().code(),
                                                  start.error().message());
        if (!end) return MaiToolResult::failure(end.error().code(),
                                                end.error().message());
        MaiCalendarEventInput event;
        event.title = arguments["title"].get<std::string>();
        event.notes = arguments.value("notes", std::string{});
        event.sessionId = context.sessionId;
        event.startsAtUtcMs = start.value();
        event.endsAtUtcMs = end.value();
        const MaiError validation = maiValidateCalendarEvent(event,
            MaiTime::getCurrentTime());
        if (validation.hasError())
            return MaiToolResult::failure(validation.code(), validation.message());
        return create_(event, context);
    }

private:
    MaiCalendarCreateHandler create_;
};

}  // namespace

void registerMaiCalendarTools(MaiToolRegistry& tools,
                              MaiCalendarCreateHandler create) {
    tools.add(std::make_unique<CalendarCreateTool>(std::move(create)));
}
