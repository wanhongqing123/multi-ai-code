#pragma once

#include <memory>

#include "MaiTool.h"

// 创建本地图片查看工具。读取范围由宿主路径策略决定，不需要修改权限。
std::unique_ptr<MaiTool> makeMaiViewImageTool();
