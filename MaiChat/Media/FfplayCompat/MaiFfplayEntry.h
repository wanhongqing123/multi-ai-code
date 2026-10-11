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
