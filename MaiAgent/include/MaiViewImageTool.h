#pragma once

#include <memory>

#include "MaiTool.h"

// 创建本地图片查看工具。它只能读取工作目录边界内的受支持图片，不需要修改权限。
std::unique_ptr<MaiTool> makeMaiViewImageTool();
