// 文件作用：定义视频人像抠像流程及模型调用契约。
// 对外类型：MaiMattingBackgroundKind、MaiVideoMattingOptions、MaiVideoMattingResult；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "MaiTool.h"
#include "MaiRvmMatting.h"

enum class MaiMattingBackgroundKind {
    Solid,
    Image,
    Blur,
};

struct MaiVideoMattingOptions {
    MaiMattingBackgroundKind backgroundKind = MaiMattingBackgroundKind::Solid;
    std::array<std::uint8_t, 3> solidRgb = {0, 0, 0};
    std::string backgroundImagePath;
    double blurSigma = 18;
    float downsampleRatio = 0.25f;
    std::string modelPath;
    std::string runtimePath;
    const void* ortApiBase = nullptr;
};

struct MaiVideoMattingResult {
    std::string outputPath;
    std::string error;
    std::string encoderName;
    int frames = 0;
    int audioStreamsCopied = 0;
    double durationSeconds = 0;
};

// 流式解码本地视频，让连续 RGB 帧通过同一个 RVM 循环会话，
// 合成新背景、编码 H.264，并在不解码的情况下复制原音频包。
// 输入视频和背景只读；处理失败或 Agent 轮次取消时删除新产物。
// 本调用会阻塞专用 Agent 工具线程，不得放在 MaiChat UI／图形线程执行。
// 宿主须在各平台打包 ONNX Runtime 1.26 和同一 RVM 模型，并在 options 中提供本地路径。
MaiVideoMattingResult maiMatteVideo(const std::string& inputPath, const std::string& outputPath,
                                    const MaiVideoMattingOptions& options,
                                    const MaiToolContext& context);
