#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiFfplayIosSession MaiFfplayIosSession;

MaiFfplayIosSession* maiFfplayIosStart(uint64_t view_id, const char* path);
int maiFfplayIosCommand(MaiFfplayIosSession* session, const char* command);
int maiFfplayIosSeekPercent(MaiFfplayIosSession* session, double fraction);
int maiFfplayIosHasFinished(const MaiFfplayIosSession* session);
int maiFfplayIosExitCode(const MaiFfplayIosSession* session);
void maiFfplayIosStop(MaiFfplayIosSession* session);

#ifdef __cplusplus
}
#endif
