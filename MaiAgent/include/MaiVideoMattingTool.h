// 文件作用：把视频人像抠像流程封装为 Agent 工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"
#include "MaiVideoMatting.h"

// 宿主打包 ONNX Runtime 与 RVM 模型后才注册此标准 Agent 工具。
// 处理器复用 MaiAgent 的 FFmpeg/OpenCV 代码，在工具线程同步执行。
// 模型与运行库路径来自可信宿主配置，绝不能由 Agent 的 JSON 参数控制。
using MaiVideoMattingProcessor = MaiVideoMattingResult (*)(const std::string& inputPath,
                                                           const std::string& outputPath,
                                                           const MaiVideoMattingOptions& options,
                                                           const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiVideoMattingTool(MaiVideoMattingProcessor processor,
                                                 std::string modelPath,
                                                 std::string runtimePath = {},
                                                 const void* apiBase = nullptr);
