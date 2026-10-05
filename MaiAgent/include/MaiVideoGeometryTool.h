#pragma once

#include <memory>
#include <cstdint>
#include <string>

#include "MaiError.h"
#include "MaiFfmpegTools.h"
#include "MaiTool.h"

// A deterministic lower-region stretch. The top region is not geometrically transformed and
// the output H.264 video uses lossless QP 0. It is restricted to 8-bit yuv420p SDR sources with
// BT.709 or unspecified color metadata; HDR and wide-gamut inputs fail rather than silently
// losing color information. Audio is stream-copied where the MP4 muxer allows it.
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

// Direct C++ entry point for hosts and batch jobs. It uses the same in-process FFprobe/FFmpeg
// engine as the model tool and resolves both paths against MaiToolContext. Existing outputs
// are never overwritten. The engine callback must remain alive for the duration of this call.
MaiResult<MaiStretchLowerResult> maiStretchLowerVideo(const MaiStretchLowerOptions& options,
                                                      MaiFfmpegEngine engine,
                                                      const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiStretchLowerVideoTool(MaiFfmpegEngine engine);
