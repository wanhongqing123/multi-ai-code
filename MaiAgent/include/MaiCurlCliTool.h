// 文件作用：将 curl 命令行能力适配为 Agent 工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// 在 Agent 工作线程执行仓库内 curl CLI 的有界、非交互式子集。
// 调用上游 argc/argv 入口前先校验全部文件路径和选项。
// 审批遵循 Agent 策略；完全访问策略下此工具不额外弹窗。
std::unique_ptr<MaiTool> makeMaiCurlCliTool(std::string caBundlePath = {});
