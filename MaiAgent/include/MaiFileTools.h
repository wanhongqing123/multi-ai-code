#pragma once

#include <memory>

#include "MaiTool.h"

// 文件类工具：读、整份覆写、按名字找、按内容找。
//
// 都受 maiResolvePathWithinRoot 那条边界管——模型只能碰会话工作目录里的东西。
std::unique_ptr<MaiTool> makeMaiReadTool();
std::unique_ptr<MaiTool> makeMaiWriteTool();
std::unique_ptr<MaiTool> makeMaiGlobTool();
std::unique_ptr<MaiTool> makeMaiGrepTool();
