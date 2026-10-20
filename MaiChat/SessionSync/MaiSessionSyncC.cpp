#include "MaiSessionSyncC.h"

#include "MaiSessionSyncSocket.h"

#include <new>
#include <string>

struct MaiSessionSyncHandle {
    MaiSessionSyncEventCallback callback = nullptr;
    void* context = nullptr;
    MaiSessionSyncSocket socket;

    MaiSessionSyncHandle(MaiSessionSyncEventCallback value, void* user)
        : callback(value), context(user), socket([this](MaiSessionSyncSocket::Event event, const std::string& detail) {
              if (callback) callback(context, static_cast<int>(event) + 1, detail.data(), detail.size());
          }) {}
};

MaiSessionSyncHandle* maiSessionSyncCreate(MaiSessionSyncEventCallback callback, void* context) {
    if (callback == nullptr) return nullptr;
    return new (std::nothrow) MaiSessionSyncHandle(callback, context);
}

int maiSessionSyncStart(MaiSessionSyncHandle* handle, const char* url, const char* bearer, const char* caPem,
                        const char* proxyUrl) {
    if (!handle || !url || !bearer || !caPem) return 0;
    try {
        return handle->socket.start({url, bearer, caPem, proxyUrl ? proxyUrl : ""}) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int maiSessionSyncSend(MaiSessionSyncHandle* handle, const char* bytes, size_t length) {
    if (!handle || !bytes || length == 0 || length > 96 * 1024) return 0;
    try {
        return handle->socket.send(std::string(bytes, length)) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int maiSessionSyncUpdateBearer(MaiSessionSyncHandle* handle, const char* bearer) {
    if (!handle || !bearer) return 0;
    return handle->socket.updateBearer(bearer) ? 1 : 0;
}

void maiSessionSyncStop(MaiSessionSyncHandle* handle) {
    if (handle) handle->socket.stop();
}

void maiSessionSyncDestroy(MaiSessionSyncHandle* handle) {
    delete handle;
}
