#pragma once

#include <memory>

#include "MaiTool.h"

// current_time：现在几点。
//
// 模型不知道今天几号。不给它的话它会按训练时的日期去推算「最近」「三天前」。
std::unique_ptr<MaiTool> makeMaiCurrentTimeTool();
