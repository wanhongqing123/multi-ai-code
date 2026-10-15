// 文件作用：提供基于 libcurl 的网络请求工具和安全参数处理。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <string>

#include "MaiTool.h"

// 注册一个面向模型的 curl 命令，由内嵌上游 CLI 执行。
// 专用 libcurl API 辅助入口仍供供应商内部调用，但不注册为模型工具。
void registerMaiCurlTools(MaiToolRegistry& registry, std::string caBundlePath = {});
