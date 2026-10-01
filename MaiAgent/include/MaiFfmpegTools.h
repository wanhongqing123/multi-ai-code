#pragma once

#include <cstddef>
#include <memory>

#include "MaiTool.h"
#include "MaiViewImageTool.h"

// The host owns the embedded FFmpeg engines and links their archives. Each
// callback runs in the caller's process, on the Agent tool worker thread.
// It must return an exit status instead of terminating the application.
// Optional log/error callbacks are called during the serialized tool invocation.
// A log sink must stop referencing its opaque pointer after setLogSink(nullptr,
// nullptr) returns. Its lines may arrive from FFmpeg worker threads.
struct MaiFfmpegEngine {
    int (*runFfmpeg)(int argc, char** argv) = nullptr;
    void (*setFfmpegCancelCheck)(int (*check)(void*), void* opaque) = nullptr;
    int (*runFfprobe)(int argc, char** argv) = nullptr;
    void (*setFfprobeCancelCheck)(int (*check)(void*), void* opaque) = nullptr;
    void (*setLogSink)(void (*sink)(void* opaque, const char* line), void* opaque) = nullptr;
    int (*describeError)(int code, char* buffer, std::size_t size) = nullptr;
};

// Do not register a factory if its matching engine callback is null. The two
// tools serialize calls because FFmpeg's original command code owns process
// globals. Cancellation is delivered to the active engine in-process.
std::unique_ptr<MaiTool> makeMaiFfmpegTool(MaiFfmpegEngine engine);
std::unique_ptr<MaiTool> makeMaiFfprobeTool(MaiFfmpegEngine engine);

// Produce a JPEG model observation at most 1280 pixels on either side, without
// changing the source image. The preview remains in the Agent workspace for
// later turns. Engine calls are serialized with the normal FFmpeg tool.
MaiImagePreviewCallback makeMaiFfmpegImagePreview(MaiFfmpegEngine engine);
MaiModelImagePreparer makeMaiFfmpegModelImagePreparer(MaiFfmpegEngine engine);
