#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiFfplayIosSession MaiFfplayIosSession;

MaiFfplayIosSession* maiFfplayIosStart(uint64_t view_id, const char* path);
MaiFfplayIosSession* maiFfplayIosStartAtSpeed(uint64_t view_id, const char* path,
                                              double speed, int64_t start_position_us);
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
