// 文件作用：声明包装 ffplay 的启动、停止、跳转和播放器状态，把宿主命令交给播放循环所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include "MaiFfplayHost.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MAI_FFPLAY_EXPORT __declspec(dllexport)
#else
#define MAI_FFPLAY_EXPORT __attribute__((visibility("default")))
#endif

// Blocks on the caller's playback thread until the hosted ffplay session
// closes. The source file included by the implementation is byte-identical to
// upstream ffplay.c. Process exit and signal handling are intercepted here.
MAI_FFPLAY_EXPORT int maiFfplayRun(const MaiFfplayHost* host, int argc, char** argv);
// Reopens the same hosted FFplay engine with a different playback rate. The caller first stops
// and joins the prior session, then passes its source-media position as start_position_us.
// Video PTS and pitch-preserving audio tempo are retimed together; no output file is created.
// Supported speed values are 0.25, 0.5, 1, 1.5, and 2. The upstream ffplay.c remains untouched.
MAI_FFPLAY_EXPORT int maiFfplayRunAtSpeed(const MaiFfplayHost* host, int argc, char** argv,
                                         double speed, int64_t start_position_us);
MAI_FFPLAY_EXPORT void maiFfplayRequestQuit(void);
// Maps an Agent/MaiChat command onto ffplay's existing control handling.
// Returns 1 if queued, 0 if playback is not active, -1 for an unknown command.
MAI_FFPLAY_EXPORT int maiFfplaySendCommand(const char* command);
// Requests an absolute source-media timestamp. The FFplay playback thread
// applies it directly, without synthesizing a mouse event from window width.
MAI_FFPLAY_EXPORT int maiFfplaySeekAbsoluteUs(int64_t position_us);
MAI_FFPLAY_EXPORT int maiFfplaySeekPercent(double fraction);
typedef struct MaiFfplayPlaybackStatus {
    int64_t position_us;
    int64_t duration_us;
    int running;
    int paused;
    int speed_percent;
} MaiFfplayPlaybackStatus;
// A thread-safe snapshot sampled on ffplay's playback/event thread.
MAI_FFPLAY_EXPORT int maiFfplayGetPlaybackStatus(MaiFfplayPlaybackStatus* status);
// Receives FFplay diagnostics from playback worker threads. The caller owns
// user_data and must clear the sink after maiFfplayRun returns.
MAI_FFPLAY_EXPORT void maiFfplaySetDiagnosticSink(
    void (*sink)(void* user_data, int level, const char* line), void* user_data);

#ifdef __cplusplus
}
#endif
