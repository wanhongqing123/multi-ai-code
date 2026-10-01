#pragma once

#include <memory>

#include "MaiTool.h"

// The host owns the embedded FFmpeg engines and links their archives. Each
// callback runs in the caller's process, on the Agent tool worker thread.
// It must return an exit status instead of terminating the application.
struct MaiFfmpegEngine {
    int (*runFfmpeg)(int argc, char** argv) = nullptr;
    void (*setFfmpegCancelCheck)(int (*check)(void*), void* opaque) = nullptr;
    int (*runFfprobe)(int argc, char** argv) = nullptr;
    void (*setFfprobeCancelCheck)(int (*check)(void*), void* opaque) = nullptr;
};

// Do not register a factory if its matching engine callback is null. The two
// tools serialize calls because FFmpeg's original command code owns process
// globals. Cancellation is delivered to the active engine in-process.
std::unique_ptr<MaiTool> makeMaiFfmpegTool(MaiFfmpegEngine engine);
std::unique_ptr<MaiTool> makeMaiFfprobeTool(MaiFfmpegEngine engine);
