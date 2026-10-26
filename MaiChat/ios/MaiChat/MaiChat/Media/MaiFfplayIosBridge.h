// 文件作用：声明 FFplay 与 iOS 播放界面之间交换帧、音频和控制状态的桥接接口。
// 连接 Swift 播放视图与 C++ FFplay 图形／音频引擎。
// 桥接函数负责播放命令、帧呈现和句柄生命周期，不持久化用户媒体。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiFfplayIosSession MaiFfplayIosSession;
typedef struct MaiCloudVod MaiCloudVod;

MaiFfplayIosSession* maiFfplayIosStart(uint64_t view_id, const char* path);
MaiFfplayIosSession* maiFfplayIosStartAtSpeed(uint64_t view_id, const char* path,
                                              double speed, int64_t start_position_us);
MaiFfplayIosSession* maiFfplayIosStartVodAtSpeed(uint64_t view_id,
                                                 MaiCloudVod* source,
                                                 double speed,
                                                 int64_t start_position_us);
int maiFfplayIosCommand(MaiFfplayIosSession* session, const char* command);
int maiFfplayIosSeekAbsoluteUs(MaiFfplayIosSession* session, int64_t position_us);
int maiFfplayIosSeekPercent(MaiFfplayIosSession* session, double fraction);
int maiFfplayIosHasFinished(const MaiFfplayIosSession* session);
int maiFfplayIosExitCode(const MaiFfplayIosSession* session);
void maiFfplayIosStop(MaiFfplayIosSession* session);
uint64_t maiFfplayAppleNonSilentBufferCount(void);

#ifdef __cplusplus
}
#endif
