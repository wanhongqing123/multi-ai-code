#include "obs.h"

bool obs_get_video_info(struct obs_video_info* video_info) {
    if (video_info) video_info->colorspace = VIDEO_CS_DEFAULT;
    return false;
}

signal_handler_t* obs_get_signal_handler(void) {
    return 0;
}

void signal_handler_connect(signal_handler_t* handler, const char* signal,
                            signal_callback_t callback, void* data) {
    (void)handler;
    (void)signal;
    (void)callback;
    (void)data;
}

void signal_handler_disconnect(signal_handler_t* handler, const char* signal,
                               signal_callback_t callback, void* data) {
    (void)handler;
    (void)signal;
    (void)callback;
    (void)data;
}
