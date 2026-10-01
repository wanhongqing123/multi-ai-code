#include "SDL.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "MaiFfplayCompatInternal.h"

struct SDL_mutex {
    std::mutex native;
};

struct SDL_cond {
    std::condition_variable native;
};

struct SDL_Thread {
    std::thread native;
    int result = 0;
};

namespace {

std::mutex sEventsMutex;
std::deque<SDL_Event> sEvents;
std::atomic<bool> sEventsActive{false};
thread_local std::string sLastError;

void setThreadName(const char* name) {
    if (!name || !*name) return;
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (length < 1) return;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), length);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

}  // namespace

extern "C" void maiFfplaySetError(const char* message) {
    sLastError = message ? message : "unknown ffplay adapter error";
}

extern "C" int SDL_Init(Uint32) {
    std::lock_guard<std::mutex> lock(sEventsMutex);
    sEvents.clear();
    sEventsActive = true;
    return 0;
}

extern "C" void SDL_Quit(void) {
    std::lock_guard<std::mutex> lock(sEventsMutex);
    sEventsActive = false;
    sEvents.clear();
}

extern "C" const char* SDL_GetError(void) {
    return sLastError.c_str();
}

extern "C" void SDL_Delay(Uint32 milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

extern "C" const char* SDL_getenv(const char* name) {
    return name ? std::getenv(name) : nullptr;
}

extern "C" int SDL_setenv(const char* name, const char* value, int overwrite) {
    if (!name || !value) return -1;
#if defined(_WIN32)
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

extern "C" int SDL_SetHint(const char*, const char*) { return 1; }

extern "C" SDL_mutex* SDL_CreateMutex(void) {
    return new (std::nothrow) SDL_mutex;
}

extern "C" void SDL_DestroyMutex(SDL_mutex* mutex) { delete mutex; }

extern "C" int SDL_LockMutex(SDL_mutex* mutex) {
    if (!mutex) return -1;
    mutex->native.lock();
    return 0;
}

extern "C" int SDL_UnlockMutex(SDL_mutex* mutex) {
    if (!mutex) return -1;
    mutex->native.unlock();
    return 0;
}

extern "C" SDL_cond* SDL_CreateCond(void) {
    return new (std::nothrow) SDL_cond;
}

extern "C" void SDL_DestroyCond(SDL_cond* condition) { delete condition; }

extern "C" int SDL_CondSignal(SDL_cond* condition) {
    if (!condition) return -1;
    condition->native.notify_one();
    return 0;
}

extern "C" int SDL_CondWait(SDL_cond* condition, SDL_mutex* mutex) {
    if (!condition || !mutex) return -1;
    std::unique_lock<std::mutex> lock(mutex->native, std::adopt_lock);
    condition->native.wait(lock);
    lock.release();
    return 0;
}

extern "C" int SDL_CondWaitTimeout(SDL_cond* condition, SDL_mutex* mutex,
                                    Uint32 milliseconds) {
    if (!condition || !mutex) return -1;
    std::unique_lock<std::mutex> lock(mutex->native, std::adopt_lock);
    const auto result = condition->native.wait_for(lock, std::chrono::milliseconds(milliseconds));
    lock.release();
    return result == std::cv_status::timeout ? 1 : 0;
}

extern "C" SDL_Thread* SDL_CreateThread(int (*function)(void*), const char* name, void* data) {
    if (!function) return nullptr;
    SDL_Thread* thread = new (std::nothrow) SDL_Thread;
    if (!thread) return nullptr;
    try {
        const std::string copiedName = name ? name : "mai-ffplay";
        thread->native = std::thread([thread, function, data, copiedName] {
            setThreadName(copiedName.c_str());
            thread->result = function(data);
        });
    } catch (...) {
        delete thread;
        maiFfplaySetError("could not start ffplay thread");
        return nullptr;
    }
    return thread;
}

extern "C" void SDL_WaitThread(SDL_Thread* thread, int* status) {
    if (!thread) return;
    if (thread->native.joinable()) thread->native.join();
    if (status) *status = thread->result;
    delete thread;
}

extern "C" Uint8 SDL_EventState(Uint32, int) { return 1; }
extern "C" void SDL_PumpEvents(void) {}

extern "C" int SDL_PeepEvents(SDL_Event* events, int count, int action,
                               Uint32 minType, Uint32 maxType) {
    if (!events || count < 1 || action != SDL_GETEVENT) return -1;
    std::lock_guard<std::mutex> lock(sEventsMutex);
    int found = 0;
    for (auto item = sEvents.begin(); item != sEvents.end() && found < count;) {
        if (item->type >= minType && item->type <= maxType) {
            events[found++] = *item;
            item = sEvents.erase(item);
        } else {
            ++item;
        }
    }
    return found;
}

extern "C" int SDL_PushEvent(SDL_Event* event) {
    if (!event) return -1;
    std::lock_guard<std::mutex> lock(sEventsMutex);
    if (!sEventsActive) return -1;
    sEvents.push_back(*event);
    return 1;
}
