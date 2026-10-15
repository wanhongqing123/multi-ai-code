// 文件作用：声明在工作线程中解码视频，并同步处理播放、暂停、跳转和帧呈现所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiVideoPlayback MaiVideoPlayback;

typedef enum MaiVideoPlaybackEvent {
    MAI_VIDEO_READY,
    MAI_VIDEO_PLAYING,
    MAI_VIDEO_PAUSED,
    MAI_VIDEO_ENDED,
    MAI_VIDEO_ERROR
} MaiVideoPlaybackEvent;

// READY、ENDED 和 ERROR 在媒体线程回调；PLAYING 和 PAUSED 在调用控制命令的线程回调。
// App 更新界面时须自行切回 UI 线程，且不得在回调内部销毁播放器对象。
typedef void (*MaiVideoPlaybackCallback)(MaiVideoPlayback* playback, MaiVideoPlaybackEvent event,
                                         void* user_data);

// 异步打开本地文件，并把解码视频帧交给已存在的 MaiGraphicsPresenter 视图。
// 这个底层播放入口不输出音频；调用方不能把它当成完整音视频播放器呈现。
MaiVideoPlayback* maiVideoPlaybackCreate(uint64_t view_id, const char* file_path,
                                         MaiVideoPlaybackCallback callback, void* user_data);
void maiVideoPlaybackDestroy(MaiVideoPlayback* playback);
bool maiVideoPlaybackPlay(MaiVideoPlayback* playback);
bool maiVideoPlaybackPause(MaiVideoPlayback* playback);
bool maiVideoPlaybackSeek(MaiVideoPlayback* playback, int64_t position_ms);
bool maiVideoPlaybackStep(MaiVideoPlayback* playback);
bool maiVideoPlaybackSetSpeed(MaiVideoPlayback* playback, double speed);
void maiVideoPlaybackSetLoop(MaiVideoPlayback* playback, bool loop);
int64_t maiVideoPlaybackPosition(const MaiVideoPlayback* playback);
int64_t maiVideoPlaybackDuration(const MaiVideoPlayback* playback);

#ifdef __cplusplus
}
#endif
