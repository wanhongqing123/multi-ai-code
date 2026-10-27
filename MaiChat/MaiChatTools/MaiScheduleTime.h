#pragma once

#include <cstdint>
#include <string>

#include "MaiError.h"

// Parse a reviewable RFC 3339 second-precision timestamp with an explicit
// UTC offset. A missing offset is rejected to avoid local-time ambiguity.
MaiResult<std::int64_t> maiParseScheduledTime(const std::string& value);
