// 文件作用：读取工作区图片并返回可供模型查看的内容。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <functional>
#include <memory>

#include "MaiTool.h"

// 可选宿主回调生成有界的模型预览并返回绝对路径。
// 文件在后续模型轮次中必须保持可读；失败应回报模型，不能改发无上限的原图。
using MaiImagePreviewCallback =
    std::function<MaiResult<std::string>(const std::string& source, const MaiToolContext& context)>;

// 创建本地图片查看工具。读取范围由宿主路径策略决定，不需要修改权限。
std::unique_ptr<MaiTool> makeMaiViewImageTool(MaiImagePreviewCallback preview = {});
