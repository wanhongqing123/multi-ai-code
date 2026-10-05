#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiTool.h"

// Paths must be host-provided app-owned directories. `workspace` is reported but never cleaned.
// The tool scans only regular files without following directory symlinks. Cleanup requires a
// recent preview ID, rechecks each file's size and modification time, and asks for fresh approval
// on every call. A manager instance retains one preview per Agent and never deletes directories.
struct MaiAppStoragePaths {
    std::string temporary;
    std::string cache;
    std::string workspace;
};

std::unique_ptr<MaiTool> makeMaiAppStorageTool(MaiAppStoragePaths paths);
