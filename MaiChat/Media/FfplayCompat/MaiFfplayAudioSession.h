// 文件作用：供各平台播放器界面共用的 FFplay 纯音频会话接口。
#pragma once

#include "MaiFfplayEntry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiFfplayAudioSession MaiFfplayAudioSession;

// 输入必须是应用可读取的本地文件路径。一个进程同时只运行一个 FFplay 会话。
MAI_FFPLAY_EXPORT MaiFfplayAudioSession* maiFfplayAudioStart(const char* path);
MAI_FFPLAY_EXPORT int maiFfplayAudioCommand(MaiFfplayAudioSession* session,
                                             const char* command);
MAI_FFPLAY_EXPORT int maiFfplayAudioSeekAbsoluteUs(MaiFfplayAudioSession* session,
                                                    int64_t position_us);
MAI_FFPLAY_EXPORT int maiFfplayAudioStatus(MaiFfplayAudioSession* session,
                                           MaiFfplayPlaybackStatus* status);
// 返回 1 表示会话结束，同时写入 FFplay 的退出码。
MAI_FFPLAY_EXPORT int maiFfplayAudioFinished(MaiFfplayAudioSession* session,
                                             int* exit_code);
MAI_FFPLAY_EXPORT void maiFfplayAudioStop(MaiFfplayAudioSession* session);

#ifdef __cplusplus
}
#endif
