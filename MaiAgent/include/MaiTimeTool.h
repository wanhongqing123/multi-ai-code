// 文件作用：向 Agent 提供当前时间查询工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiTool.h"

// current_time：现在几点。
//
// 模型不知道今天几号。不给它的话它会按训练时的日期去推算「最近」「三天前」。
std::unique_ptr<MaiTool> makeMaiCurrentTimeTool();
