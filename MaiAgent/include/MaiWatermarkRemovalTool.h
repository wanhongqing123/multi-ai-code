#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MaiFfmpegTools.h"
#include "MaiTool.h"

// 水印矩形采用 FFprobe 报告的原始视频像素坐标。时间窗只用于视频；endSeconds 为
// -1 时从 startSeconds 一直处理到片尾。一次最多传四个矩形，避免模型反复试探。
struct MaiWatermarkRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double startSeconds = 0;
    double endSeconds = -1;
};

// 输入可为 MP4/MOV 或 JPEG/PNG；输出视频必须为新的 MP4，输出图片为新的 JPEG/PNG。
// 调用只在工具工作线程执行，FFmpeg 引擎由宿主持有；原文件绝不覆盖。该算法从
// 矩形周围估算填充像素，无法保证被遮盖的真实内容恢复。区域需由调用方明确给出。
struct MaiWatermarkRemovalOptions {
    std::string inputPath;
    std::string outputPath;
    std::vector<MaiWatermarkRegion> regions;
};

struct MaiWatermarkRemovalResult {
    std::string outputPath;
    int width = 0;
    int height = 0;
    std::uint64_t outputBytes = 0;
    bool video = false;
};

// 与模型工具共用同一实现：先检查路径、尺寸、色彩和区域，再各调用一次内嵌
// FFprobe/FFmpeg。失败时清理本次临时产物，不创建最终文件；取消也不会覆盖原图。
MaiResult<MaiWatermarkRemovalResult> maiRemoveWatermark(const MaiWatermarkRemovalOptions& options,
                                                        MaiFfmpegEngine engine,
                                                        const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiWatermarkRemovalTool(MaiFfmpegEngine engine);
