#pragma once

#include <memory>

#include "MaiTool.h"

// todowrite：模型自己记的任务清单。
//
// **无状态**：清单不存在任何地方，模型每次传完整的，工具只校验再摆回去。
// 清单本身活在对话历史里。
std::unique_ptr<MaiTool> makeMaiTodoWriteTool();
