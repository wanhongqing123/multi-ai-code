// C boundary for the shared Session WebSocket transport on iOS and Android.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiSessionSyncHandle MaiSessionSyncHandle;
typedef void (*MaiSessionSyncEventCallback)(void* context, int event, const char* data, size_t length);

// Events: 1 connected, 2 disconnected, 3 text message, 4 transport error.
// The callback runs on the transport thread and may not destroy its handle.
MaiSessionSyncHandle* maiSessionSyncCreate(MaiSessionSyncEventCallback callback, void* context);
int maiSessionSyncStart(MaiSessionSyncHandle* handle, const char* url, const char* bearer, const char* caPem,
                        const char* proxyUrl);
int maiSessionSyncUpdateBearer(MaiSessionSyncHandle* handle, const char* bearer);
int maiSessionSyncSend(MaiSessionSyncHandle* handle, const char* bytes, size_t length);
void maiSessionSyncStop(MaiSessionSyncHandle* handle);
void maiSessionSyncDestroy(MaiSessionSyncHandle* handle);

#ifdef __cplusplus
}
#endif
