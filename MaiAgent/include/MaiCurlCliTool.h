// 文件作用：将 curl 命令行能力适配为 Agent 工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// Execute a bounded, non-interactive subset of the vendored curl CLI in the Agent worker thread.
// The tool validates all file paths and options before it calls the upstream argc/argv entry.
// Approval follows the Agent policy. The full-access policy does not prompt for this tool.
std::unique_ptr<MaiTool> makeMaiCurlCliTool(std::string caBundlePath = {});
