#pragma once

#include <memory>

#include "MaiTool.h"

// 创建屏幕截图工具。工具始终需要用户审批；不支持原生捕获的平台不会在内置工具表中注册它。
std::unique_ptr<MaiTool> makeMaiScreenshotTool();
