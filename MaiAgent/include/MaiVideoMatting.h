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
    int frames = 0;
    int audioStreamsCopied = 0;
    double durationSeconds = 0;
};

// Stream-decodes a local video, passes consecutive RGB frames through one RVM recurrent session,
// composites a new background, encodes H.264, and packet-copies the original audio without
// decoding it. Input and background files are read-only. The new output is removed if processing
// fails or the Agent turn is cancelled. This call blocks its dedicated Agent tool worker; it must
// not run on the MaiChat UI/graphics thread. The host must bundle ONNX Runtime 1.26 and the same
// RVM model on each platform and supply their local paths in options.
MaiVideoMattingResult maiMatteVideo(const std::string& inputPath, const std::string& outputPath,
                                    const MaiVideoMattingOptions& options,
                                    const MaiToolContext& context);
