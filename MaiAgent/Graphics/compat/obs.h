#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The standalone Graphics build does not include OBS's application/video
// subsystem. These declarations cover only the callbacks used by its Metal
// backend; they are not a replacement for the public libobs API.
enum video_colorspace {
    VIDEO_CS_DEFAULT,
    VIDEO_CS_601,
    VIDEO_CS_709,
    VIDEO_CS_SRGB,
    VIDEO_CS_2100_PQ,
    VIDEO_CS_2100_HLG,
};

struct obs_video_info {
    enum video_colorspace colorspace;
};

struct calldata {
    uint8_t* stack;
    size_t size;
    size_t capacity;
    bool fixed;
};
struct signal_handler;
typedef struct signal_handler signal_handler_t;
typedef void (*signal_callback_t)(void*, struct calldata*);

#ifdef __cplusplus
extern "C" {
#endif

bool obs_get_video_info(struct obs_video_info* video_info);
signal_handler_t* obs_get_signal_handler(void);
void signal_handler_connect(signal_handler_t* handler, const char* signal,
                            signal_callback_t callback, void* data);
void signal_handler_disconnect(signal_handler_t* handler, const char* signal,
                               signal_callback_t callback, void* data);

#ifdef __cplusplus
}
#endif
