#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// Supplies the Ark API key at execution time. Hosts retain ownership of the secret and may
// replace or revoke it without rebuilding the Agent. The callback must be safe to call from a
// tool worker; an empty value leaves discovery available but disables task submission.
using MaiArkApiKeyProvider = std::function<std::string()>;

// Shared Seedance video tool. It owns task submission, status polling, and workspace downloads.
// Local video upload remains unavailable until a host supplies a secure upload route. Calls may
// block on network I/O and honor MaiToolContext cancellation; no UI thread may execute them.
std::unique_ptr<MaiTool> makeMaiSeedanceVideoTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath = {});

// Shared Seedream image tool. It reads one accessible image, sends it to Ark, and creates a new
// PNG below the Agent workspace. The source is never modified. The same threading, cancellation,
// and key lifetime contract as makeMaiSeedanceVideoTool applies.
std::unique_ptr<MaiTool> makeMaiSeedreamImageTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath = {});
