#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// The host supplies its currently selected GLM credential when a tool runs. The key never enters
// tool metadata, persisted tasks, or model-visible output. The callback must be worker-safe.
using MaiGlmApiKeyProvider = std::function<std::string()>;

// Paid asynchronous generation tools. They submit a task, persist its ID when a specialist task
// store is present, and poll/download into the Agent workspace. Calls block on network I/O and
// honor cancellation. They must never run on a UI thread. An empty key disables submission while
// leaving discovery available; provider entitlement is verified only by an actual API response.
std::unique_ptr<MaiTool> makeMaiGlmVideoTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath = {});
std::unique_ptr<MaiTool> makeMaiGlmImageTool(MaiGlmApiKeyProvider apiKey,
                                             std::string caBundlePath = {});
