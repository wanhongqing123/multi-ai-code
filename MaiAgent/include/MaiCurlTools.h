// 文件作用：提供基于 libcurl 的网络请求工具和安全参数处理。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <string>

#include "MaiTool.h"

// Register one model-facing curl command backed by the embedded upstream CLI. Specialized
// libcurl API helpers remain available for internal provider use but are not registered tools.
void registerMaiCurlTools(MaiToolRegistry& registry, std::string caBundlePath = {});
