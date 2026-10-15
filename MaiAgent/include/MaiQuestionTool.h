// 文件作用：将向用户提问的能力暴露为 Agent 工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiTool.h"

// question：中途问用户一句，等回答，然后接着干。
//
// 要 MaiToolContext 上挂着问答闸门才能用；没挂的话它会明说问不了，而不是干等。
std::unique_ptr<MaiTool> makeMaiQuestionTool();
