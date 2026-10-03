#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiTool.h"

struct MaiAgentMedia {
    std::string relativePath;
    std::string type;
    std::string mimeType;
    std::uint64_t bytes = 0;
};

// Validate a regular file in the current Agent workspace for persistent display in this
// conversation. The input must be relative to context.root; symbolic-link escapes are rejected.
// This does not send anything to an IM contact or modify the source file. The returned path is
// stable for the session as long as the workspace file exists. It is safe to call on a worker
// thread and does not retain the per-turn context.
MaiResult<MaiAgentMedia> maiPrepareAgentMedia(const std::string& filePath, const std::string& type,
                                              const MaiToolContext& context);

// The tool's completed output is persisted as a normal assistant tool part. MaiChat renders that
// part as an image, video, or audio card in the current AI conversation; no peer_id is involved.
std::unique_ptr<MaiTool> makeMaiAgentSendMediaTool();
