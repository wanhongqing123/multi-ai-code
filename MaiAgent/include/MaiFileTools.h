#pragma once

#include <memory>

#include "MaiTool.h"

// 文件类工具：读、显式创建文件/目录、删除文件、整份覆写、查找。
//
// 相对路径从会话工作目录解析；实际访问范围由宿主通过 MaiToolContext 设置。
std::unique_ptr<MaiTool> makeMaiReadTool();
std::unique_ptr<MaiTool> makeMaiCreateFileTool();
std::unique_ptr<MaiTool> makeMaiCreateDirectoryTool();
std::unique_ptr<MaiTool> makeMaiDeleteFileTool();
std::unique_ptr<MaiTool> makeMaiWriteTool();
std::unique_ptr<MaiTool> makeMaiGlobTool();
std::unique_ptr<MaiTool> makeMaiGrepTool();

// Register the workspace file tools as one capability family. Implementations stay in their
// paired files: plain file operations here, exact edits in MaiEditTool, and atomic multi-file
// patches in MaiApplyPatchTool. Tool names and permission behavior remain unchanged.
void registerMaiFileTools(MaiToolRegistry& registry);
