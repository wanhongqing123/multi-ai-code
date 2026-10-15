// 文件作用：列举可见窗口及其基础信息。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiTool.h"

// 创建可捕获窗口枚举工具。窗口标题属于屏幕隐私信息，因此每次首次调用都经过审批闸门。
std::unique_ptr<MaiTool> makeMaiWindowListTool();
