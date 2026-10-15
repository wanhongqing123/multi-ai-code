// 文件作用：分析视频画面几何信息并生成裁剪参数。
// 对外类型：MaiStretchLowerOptions、MaiStretchLowerResult；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <memory>
#include <cstdint>
#include <string>

#include "MaiError.h"
#include "MaiFfmpegTools.h"
#include "MaiTool.h"

// 确定性地拉伸画面下半部，上半部不做几何变形；H.264 输出采用无损 QP 0。
// 仅支持 8 位 yuv420p SDR，色彩元数据须为 BT.709 或未指定；
// HDR 和宽色域输入直接失败，不能静默丢失色彩信息。
// MP4 封装允许时直接复制原音频流。
struct MaiStretchLowerOptions {
    std::string inputPath;
    std::string outputPath;
    int splitY = 0;
    double factor = 1.1;
};

struct MaiStretchLowerResult {
    std::string outputPath;
    int inputWidth = 0;
    int inputHeight = 0;
    int outputHeight = 0;
    std::uint64_t outputBytes = 0;
};

// 供宿主与批处理调用的 C++ 直接入口，与模型工具共用进程内 FFprobe/FFmpeg 引擎。
// 两条路径都按 MaiToolContext 解析，绝不覆盖已有输出。
// 引擎回调必须在整个调用期间保持有效。
MaiResult<MaiStretchLowerResult> maiStretchLowerVideo(const MaiStretchLowerOptions& options,
                                                      MaiFfmpegEngine engine,
                                                      const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiStretchLowerVideoTool(MaiFfmpegEngine engine);
