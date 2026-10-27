#pragma once

#include <functional>

#include <QUrl>

#include "MaiCalendarTools.h"

// Writes a standards-based calendar invitation in the Agent workspace, then
// opens the OS calendar importer when one is available. Import still needs the
// user's confirmation; the result never claims that an event was saved.
MaiToolResult prepareDesktopCalendarImport(const MaiCalendarEventInput& event,
    const MaiToolContext& context,
    std::function<bool(const QUrl&)> open = {});
