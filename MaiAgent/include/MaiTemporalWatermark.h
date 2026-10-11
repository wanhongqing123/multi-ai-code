#pragma once

#include <string>

#include "MaiTemporalWatermarkTool.h"

// 端侧同步处理器。逐帧解码 MP4/MOV，局部使用 LaMa ONNX 或前后干净帧补画，
// 再编码 H.264 并直接复制兼容 MP4 的原音轨。调用方须在 Agent 工作线程运行。
// 输入只读，输出不存在；失败、取消时清理本次输出，不暴露半成品。
MaiTemporalWatermarkResult maiRepairVideoWatermarks(const std::string& inputPath,
                                                    const std::string& outputPath,
                                                    const MaiTemporalWatermarkOptions& options,
                                                    const MaiToolContext& context);
