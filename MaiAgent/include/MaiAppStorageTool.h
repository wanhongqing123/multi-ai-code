// 文件作用：提供应用私有存储的查询与文件操作工具。
// 对外类型：MaiAppStoragePaths；线程、失败和生命周期以类型附近注释为准。
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
