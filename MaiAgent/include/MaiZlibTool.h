// 文件作用：提供压缩和解压缩文件的工具能力。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

// 按指定 zlib 流格式压缩或解压内存字节。"auto" 只可用于解压，识别 gzip 与 zlib 头。
// 输入输出由调用方设限；超过 maxOutputBytes 返回 InvalidInput，不返回部分结果。
// 可并发调用，不访问文件、全局状态或模型状态。
MaiResult<std::string> maiTransformZlibBytes(const std::string& input, bool compress,
                                             const std::string& format, std::size_t maxOutputBytes);

// 两个工具读取现有工作区文件，再创建不覆盖旧文件的新文件，共用上面的字节变换。
// 路径经 MaiToolContext 解析，不能越过宿主配置的文件边界；文件修改须经用户审批。
std::unique_ptr<MaiTool> makeMaiZlibCompressTool();
std::unique_ptr<MaiTool> makeMaiZlibDecompressTool();
