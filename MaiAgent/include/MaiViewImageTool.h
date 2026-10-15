// 文件作用：读取工作区图片并返回可供模型查看的内容。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <functional>
#include <memory>

#include "MaiTool.h"

// The optional host callback creates a bounded model preview and returns its
// absolute path. It must keep that file readable for subsequent model turns;
// failure is returned to the model instead of sending an unbounded original.
using MaiImagePreviewCallback =
    std::function<MaiResult<std::string>(const std::string& source, const MaiToolContext& context)>;

// 创建本地图片查看工具。读取范围由宿主路径策略决定，不需要修改权限。
std::unique_ptr<MaiTool> makeMaiViewImageTool(MaiImagePreviewCallback preview = {});
