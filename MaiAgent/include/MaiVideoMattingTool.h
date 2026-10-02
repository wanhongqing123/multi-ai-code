#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"
#include "MaiVideoMatting.h"

// The host installs this standard Agent tool only after ONNX Runtime and the RVM model are bundled.
// The processor uses shared MaiAgent FFmpeg/OpenCV code and runs synchronously on the tool worker.
// The model/runtime paths are trusted host configuration, never Agent-controlled JSON arguments.
using MaiVideoMattingProcessor = MaiVideoMattingResult (*)(const std::string& inputPath,
                                                           const std::string& outputPath,
                                                           const MaiVideoMattingOptions& options,
                                                           const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiVideoMattingTool(MaiVideoMattingProcessor processor,
                                                 std::string modelPath,
                                                 std::string runtimePath = {},
                                                 const void* apiBase = nullptr);
