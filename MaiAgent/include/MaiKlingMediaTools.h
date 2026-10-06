#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// The host supplies the Kling API key on demand from secure storage. The callback must be
// worker-safe. An empty key leaves discovery available but disables paid submission.
using MaiKlingApiKeyProvider = std::function<std::string()>;

// Kling video and image are separate model-backed tools. Both honor cancellation and publish
// downloaded results into the Agent workspace; video tasks can resume through the task store.
std::unique_ptr<MaiTool> makeMaiKlingVideoTool(MaiKlingApiKeyProvider apiKey,
                                               std::string caBundlePath = {});
std::unique_ptr<MaiTool> makeMaiKlingImageTool(MaiKlingApiKeyProvider apiKey,
                                               std::string caBundlePath = {});
