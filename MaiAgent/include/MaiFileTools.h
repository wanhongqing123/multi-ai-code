// 文件作用：提供工作区文件的读取、列举和写入工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
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

// 将工作区文件工具作为一组能力注册。普通文件操作在本组实现，
// 精确编辑由 MaiEditTool 负责，原子多文件补丁由 MaiApplyPatchTool 负责。
// 工具名称和权限行为保持不变。
void registerMaiFileTools(MaiToolRegistry& registry);
