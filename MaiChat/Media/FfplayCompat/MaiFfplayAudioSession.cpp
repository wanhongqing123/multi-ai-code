// 文件作用：以 FFplay 的 -nodisp 模式播放音频；三端 UI 共享同一状态与控制实现。
#include "MaiFfplayAudioSession.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

struct MaiFfplayAudioSession {
    std::string path;
    std::thread worker;
    std::atomic<bool> finished{false};
    std::atomic<int> exitCode{-1};
    std::atomic<int> pendingPaused{-1};
    std::atomic<int64_t> pendingPauseDeadlineNs{0};
};

namespace {
int64_t monotonicNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

extern "C" MaiFfplayAudioSession* maiFfplayAudioStart(const char* path) {
    if (!path || !*path) return nullptr;
    auto* session = new MaiFfplayAudioSession;
    session->path = path;
    try {
        session->worker = std::thread([session] {
            MaiFfplayHost host{};
            // -nodisp 不创建窗口。FFplay 当前要求一个非零的宿主视图标识。
            host.graphics_view_id = 1;
            char name[] = "ffplay";
            char noDisplay[] = "-nodisp";
            char autoExit[] = "-autoexit";
            char* arguments[] = {name, noDisplay, autoExit, session->path.data()};
            session->exitCode = maiFfplayRun(&host, 4, arguments);
            session->finished = true;
        });
    } catch (...) {
        delete session;
        return nullptr;
    }
    return session;
}

extern "C" int maiFfplayAudioCommand(MaiFfplayAudioSession* session,
                                       const char* command) {
    if (!session || session->finished || !command ||
        (std::strcmp(command, "pause") != 0 &&
         std::strcmp(command, "close") != 0)) return 0;
    int desiredPaused = -1;
    if (std::strcmp(command, "pause") == 0) {
        MaiFfplayPlaybackStatus status{};
        if (maiFfplayGetPlaybackStatus(&status) != 1) return 0;
        const int pending = session->pendingPaused.load();
        desiredPaused = 1 - (pending >= 0 ? pending : status.paused);
    }
    const int accepted = desiredPaused >= 0 ? maiFfplayRequestPauseToggle() :
        maiFfplaySendCommand(command) == 1;
    if (!accepted) return 0;
    if (desiredPaused >= 0) {
        session->pendingPaused = desiredPaused;
        session->pendingPauseDeadlineNs = monotonicNanoseconds() + 1'000'000'000;
    }
    return 1;
}

extern "C" int maiFfplayAudioSeekAbsoluteUs(MaiFfplayAudioSession* session,
                                              int64_t position_us) {
    return session && !session->finished && position_us >= 0 &&
        maiFfplaySeekAbsoluteUs(position_us) == 1 ? 1 : 0;
}

extern "C" int maiFfplayAudioStatus(MaiFfplayAudioSession* session,
                                     MaiFfplayPlaybackStatus* status) {
    if (!session || session->finished || !status ||
        maiFfplayGetPlaybackStatus(status) != 1) return 0;
    const int pending = session->pendingPaused.load();
    if (pending >= 0) {
        if (status->paused == pending ||
            monotonicNanoseconds() >= session->pendingPauseDeadlineNs.load())
            session->pendingPaused = -1;
        else
            status->paused = pending;
    }
    return 1;
}

extern "C" int maiFfplayAudioFinished(MaiFfplayAudioSession* session,
                                        int* exit_code) {
    if (!session || !session->finished) return 0;
    if (exit_code) *exit_code = session->exitCode;
    return 1;
}

extern "C" void maiFfplayAudioStop(MaiFfplayAudioSession* session) {
    if (!session) return;
    for (int attempt = 0; attempt < 100 && !session->finished; ++attempt) {
        if (maiFfplayAudioCommand(session, "close") == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (session->worker.joinable()) session->worker.join();
    delete session;
}
