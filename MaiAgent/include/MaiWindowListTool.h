#pragma once

#include <memory>

#include "MaiTool.h"

// 创建可捕获窗口枚举工具。窗口标题属于屏幕隐私信息，因此每次首次调用都经过审批闸门。
std::unique_ptr<MaiTool> makeMaiWindowListTool();
