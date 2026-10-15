// 文件作用：把屏幕截图能力封装为 Agent 工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiTool.h"

// 创建屏幕截图工具。工具始终需要用户审批；不支持原生捕获的平台不会在内置工具表中注册它。
std::unique_ptr<MaiTool> makeMaiScreenshotTool();
