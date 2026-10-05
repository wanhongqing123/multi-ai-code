#pragma once

#include <optional>

#include "MaiSystemResourcesTool.h"

// Returns GPU counters exposed by the current desktop platform. Missing metrics remain absent.
std::optional<MaiGpuResources> maiDesktopGpuResources();
