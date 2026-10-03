#pragma once

#include <stdint.h>

#include "../MaiGraphicsPresenter.h"

#ifdef __cplusplus
extern "C" {
#endif

// One hosted ffplay session is active per process, matching ffplay.c's global
// playback state. MaiChat creates the native popup and Graphics view first,
// then binds its view ID before starting the playback thread. Window callbacks
// must dispatch to the platform UI thread; the adapter never owns a window.
typedef struct MaiFfplayHost {
    void* user_data;
    uint64_t graphics_view_id;
    int present_native_frames;
    bool (*present_video)(void* user_data, const MaiVideoFrame* frame,
                          const MaiVideoSubtitle* subtitle);
    bool (*present_rgba)(void* user_data, const uint8_t* pixels,
                         uint32_t width, uint32_t height, uint32_t stride);
    void (*set_title)(void* user_data, const char* title);
    void (*set_size)(void* user_data, int width, int height);
    void (*set_position)(void* user_data, int x, int y);
    void (*set_fullscreen)(void* user_data, int enabled);
    void (*show_window)(void* user_data);
} MaiFfplayHost;

void maiFfplayBindHost(const MaiFfplayHost* host);
MaiFfplayHost maiFfplayCurrentHost(void);
int maiFfplaySourceHasHdrWithoutSubtitles(const char* path);

#ifdef __cplusplus
}
#endif
