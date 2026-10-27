#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

struct MaiCalendarEventInput {
    std::string title;
    std::string notes;
    std::string sessionId;
    std::int64_t startsAtUtcMs = 0;
    std::int64_t endsAtUtcMs = 0;
};

MaiError maiValidateCalendarEvent(const MaiCalendarEventInput& event,
                                  std::int64_t nowMs);
using MaiCalendarCreateHandler = std::function<MaiToolResult(
    const MaiCalendarEventInput& event, const MaiToolContext& context)>;

// Platform hosts use the same validated event contract for ordinary UI calls
// and the Agent-facing tool. The host owns OS permission and import UX.
void registerMaiCalendarTools(MaiToolRegistry& tools,
                              MaiCalendarCreateHandler create);
