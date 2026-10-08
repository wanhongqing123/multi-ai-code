#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// The host owns Ark Assets credentials and any private storage upload. The callback receives
// validated JSON arguments from a worker thread and returns one JSON object from the Assets
// service. A local image path, when present, has already been resolved within the tool context.
// It may block on network I/O; the host must not run it on a UI thread. An empty provider leaves
// discovery available but makes remote actions unavailable. The caller owns provider lifetime.
using MaiArkAssetProvider =
    std::function<MaiResult<std::string>(const std::string&, const MaiToolContext&)>;

struct MaiArkAssetServiceSettings {
    std::string baseUrl;
    std::string token;
};

// The host supplies its private service settings from secure storage on each call. The returned
// provider submits validated asset actions to that service. For a local image, it obtains a
// short-lived upload grant, sends file bytes directly to object storage, then registers the read
// URL with Ark. Neither the service token nor signed URLs are exposed to the model.
MaiArkAssetProvider makeMaiArkAssetServiceProvider(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath = {});

// Create/list virtual asset groups, upload an image and inspect the asynchronous asset state.
// Only an Active asset can be handed to Seedance as asset://<ID>. Upload never edits the source.
std::unique_ptr<MaiTool> makeMaiArkAssetTool(MaiArkAssetProvider provider = {});
