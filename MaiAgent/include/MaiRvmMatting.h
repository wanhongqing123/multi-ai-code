#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"

struct MaiRvmMattingResult {
    std::vector<float> foregroundRgbPlanar;
    std::vector<float> alpha;
};

// One RVM MobileNetV3 ONNX inference session for one video stream. Calls must be serialized in
// display order on the same worker thread; the four recurrent tensors are retained between frames
// and reset only when a new video begins. The caller owns the RGB32F planar [1,3,H,W] buffer for
// the duration of matte(); pixels must be normalized to [0,1]. The returned alpha is [H,W] and
// has no ownership relationship with the input. Invalid dimensions or unavailable runtimes return
// an error without mutating the source frame.
//
// runtimePath names a bundled ONNX Runtime 1.26 library. An empty path only uses an already-linked
// OrtGetApiBase symbol in the host process; it never searches arbitrary system directories.
class MaiRvmMattingSession {
public:
    static MaiResult<std::unique_ptr<MaiRvmMattingSession>> open(
        const std::string& modelPath, const std::string& runtimePath = {},
        const void* apiBase = nullptr);
    ~MaiRvmMattingSession();

    MaiRvmMattingSession(const MaiRvmMattingSession&) = delete;
    MaiRvmMattingSession& operator=(const MaiRvmMattingSession&) = delete;

    MaiResult<std::vector<float>> matte(float* rgbPlanar, int width, int height,
                                        float downsampleRatio = 0.25f);
    // Returns RVM's foreground RGB prediction and alpha for edge decontamination. Opaque
    // foreground pixels can be taken directly from the decoded source to preserve their color.
    MaiResult<MaiRvmMattingResult> infer(float* rgbPlanar, int width, int height,
                                         float downsampleRatio = 0.25f);
    MaiError reset();
    std::string runtimeVersion() const;

private:
    MaiRvmMattingSession() = default;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
