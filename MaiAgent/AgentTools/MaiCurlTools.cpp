// 文件作用：提供基于 libcurl 的网络请求工具和安全参数处理。
// 直接依赖：MaiCurlCliTool.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiCurlTools.h"

#include <utility>

#include "MaiCurlCliTool.h"

void registerMaiCurlTools(MaiToolRegistry& registry, std::string caBundlePath) {
    registry.add(makeMaiCurlCliTool(std::move(caBundlePath)));
}
