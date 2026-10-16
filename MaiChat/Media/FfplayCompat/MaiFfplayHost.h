// 文件作用：声明定义播放器向宿主报告画面、音频和播放状态的回调边界所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <stdint.h>

#include "../MaiGraphicsPresenter.h"

#if defined(_WIN32)
#define MAI_FFPLAY_EXPORT __declspec(dllexport)
#else
#define MAI_FFPLAY_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ffplay.c 使用进程全局播放状态，因此一个进程同时只有一个宿主播放会话。
// MaiChat 先创建原生弹窗与图形视图，再绑定视图 ID，最后启动播放线程。
// 窗口回调须切到平台 UI 线程；适配层本身不持有窗口。
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
MAI_FFPLAY_EXPORT int maiFfplaySourceHasHdrWithoutSubtitles(const char* path);

#ifdef __cplusplus
}
#endif
