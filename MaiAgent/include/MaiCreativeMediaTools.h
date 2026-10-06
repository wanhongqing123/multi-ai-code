#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// Credentials are read from the host on each call. The callback must be safe to invoke from a
// worker thread. Empty credentials leave discovery available but disable paid submissions.
using MaiCreativeApiKeyProvider = std::function<std::string()>;

// These tools submit paid provider tasks, persist their IDs when a specialist task store exists,
// and deliver completed media to the Agent workspace. Network calls block the calling worker and
// honor cancellation. The key is never included in metadata, task records, or model-visible output.
std::unique_ptr<MaiTool> makeMaiKlingVideoTool(MaiCreativeApiKeyProvider apiKey,
                                               std::string caBundlePath = {});
std::unique_ptr<MaiTool> makeMaiKlingImageTool(MaiCreativeApiKeyProvider apiKey,
                                               std::string caBundlePath = {});
std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiCreativeApiKeyProvider apiKey,
                                                 std::string caBundlePath = {});
std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiCreativeApiKeyProvider apiKey,
                                                 std::string caBundlePath = {});
