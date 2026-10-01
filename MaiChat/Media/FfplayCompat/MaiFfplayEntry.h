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
MAI_FFPLAY_EXPORT void maiFfplayRequestQuit(void);
// Maps an Agent/MaiChat command onto ffplay's existing control handling.
// Returns 1 if queued, 0 if playback is not active, -1 for an unknown command.
MAI_FFPLAY_EXPORT int maiFfplaySendCommand(const char* command);
MAI_FFPLAY_EXPORT int maiFfplaySeekPercent(double fraction);

#ifdef __cplusplus
}
#endif
