#pragma once

#include <memory>
#include <string>

#include "MaiFaceBeautify.h"

// Register only when both verified ONNX files and the ONNX Runtime host are bundled. The tool
// accepts an image path and creates a PNG under the current Agent workspace. The optional
// output_path is relative to that workspace. Preset and explicit effects are additive, clamped
// to [0, 1], then multiplied by strength. It never changes the input image.
std::unique_ptr<MaiTool> makeMaiFaceBeautifyTool(MaiFaceBeautifyProcessor processor,
                                                 std::string detectorModelPath,
                                                 std::string landmarkModelPath,
                                                 std::string runtimePath, const void* ortApiBase,
                                                 MaiFaceImageDecoder decodeImage = nullptr,
                                                 MaiFacePngEncoder encodePng = nullptr);
