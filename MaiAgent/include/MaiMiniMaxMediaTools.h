#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiCreativeMediaSupport.h"

// The host supplies its MiniMax API key on demand from secure storage. The callback must be
// worker-safe. An empty key leaves discovery available but disables paid submission.
using MaiMiniMaxApiKeyProvider = std::function<std::string()>;

// MiniMax video uses H3/H3 Max V2 generation. MiniMax image uses image-01 and completes
// synchronously. Both are paid and honor cancellation.
std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath = {},
                                                 MaiCreativeMediaUploadProvider uploadMedia = {});
std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath = {},
                                                 MaiCreativeMediaUploadProvider uploadMedia = {});
