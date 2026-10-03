#pragma once

#include <string>
#include <vector>

#include "MaiTool.h"

struct MaiFaceDecodedImage {
    std::vector<unsigned char> rgba;
    std::vector<unsigned char> iccProfile;
    int width = 0;
    int height = 0;
    int colorPrimaries = 0;
    int colorTransfer = 0;
};

using MaiFaceImageDecoder = bool (*)(const std::string& path, MaiFaceDecodedImage& image);
using MaiFacePngEncoder = bool (*)(const unsigned char* rgba, int width, int height, int stride,
                                   const MaiFaceDecodedImage& source,
                                   std::vector<unsigned char>& png);

struct MaiFaceBeautifyOptions {
    float smooth = 0;
    float whiten = 0;
    float slimFace = 0;
    float enlargeEye = 0;
    std::string detectorModelPath;
    std::string landmarkModelPath;
    std::string runtimePath;
    const void* ortApiBase = nullptr;
    MaiFaceImageDecoder decodeImage = nullptr;
    MaiFacePngEncoder encodePng = nullptr;
};

struct MaiFaceBeautifyResult {
    std::string outputPath;
    std::string errorCode;
    std::string errorMessage;
    int faceCount = 0;
};

// Processes a local still image into a new PNG. The caller owns both paths and must ensure the
// output does not already exist. The function runs synchronously on a tool worker, never a UI
// thread. It returns face_not_detected when no usable dense landmarks are found. Failure and
// cancellation leave no output file behind. All platforms use the same ONNX models and C++ image
// processing; ortApiBase may point to a host-linked ONNX Runtime API on mobile.
using MaiFaceBeautifyProcessor = MaiFaceBeautifyResult (*)(const std::string& inputPath,
                                                           const std::string& outputPath,
                                                           const MaiFaceBeautifyOptions& options,
                                                           const MaiToolContext& context);

MaiFaceBeautifyResult maiBeautifyFaceImage(const std::string& inputPath,
                                           const std::string& outputPath,
                                           const MaiFaceBeautifyOptions& options,
                                           const MaiToolContext& context);
