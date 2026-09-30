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

// READY, ENDED and ERROR run on the media thread. PLAYING and PAUSED run on
// the command-calling thread. The application must dispatch UI changes to its
// UI thread and must not destroy the playback object inside a callback.
typedef void (*MaiVideoPlaybackCallback)(MaiVideoPlayback* playback, MaiVideoPlaybackEvent event,
                                         void* user_data);

// Opens a local file asynchronously and sends decoded video frames to an
// existing MaiGraphicsPresenter view. This first stage has no audio output;
// callers must not present it as a complete audio/video player.
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
