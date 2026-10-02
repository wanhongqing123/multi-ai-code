#include "MaiFfplayIosBridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../../../../Media/FfplayCompat/MaiFfplayEntry.h"
#include "../../../../Media/MaiGraphicsPresenter.h"

struct MaiFfplayIosSession {
    uint64_t viewId = 0;
    std::string path;
    MaiFfplayHost host{};
    std::thread worker;
    std::atomic<bool> finished{false};
    std::atomic<int> result{0};
    std::atomic<bool> firstFrameLogged{false};
};

namespace {

bool presentVideo(void* user, const MaiVideoFrame* frame,
                  const MaiVideoSubtitle* subtitle) {
    auto* session = static_cast<MaiFfplayIosSession*>(user);
#if !defined(NDEBUG)
    if (frame && !session->firstFrameLogged.exchange(true)) {
        std::fprintf(stderr, "[FFplayFrame] %ux%u format=%d colorspace=%d range=%d\n",
                     frame->width, frame->height, static_cast<int>(frame->format),
                     static_cast<int>(frame->color_space), static_cast<int>(frame->color_range));
    }
#endif
    return maiGraphicsPresenterShowVideoFrameWithSubtitle(session->viewId,
                                                          frame, subtitle, false);
}

bool presentRgba(void* user, const uint8_t* pixels,
                 uint32_t width, uint32_t height, uint32_t stride) {
    auto* session = static_cast<MaiFfplayIosSession*>(user);
#if !defined(NDEBUG)
    if (!session->firstFrameLogged.exchange(true))
        std::fprintf(stderr, "[FFplayFrame] %ux%u format=RGBA8 stride=%u\n",
                     width, height, stride);
#endif
    return maiGraphicsPresenterShowFrame(session->viewId, pixels, width,
                                         height, stride, false);
}

}  // namespace

extern "C" MaiFfplayIosSession* maiFfplayIosStart(uint64_t viewId, const char* path) {
    if (!viewId || !path || !*path) return nullptr;
    auto* session = new MaiFfplayIosSession;
    session->viewId = viewId;
    session->path = path;
    session->host.user_data = session;
    session->host.graphics_view_id = viewId;
    session->host.present_video = presentVideo;
    session->host.present_rgba = presentRgba;
    try {
        session->worker = std::thread([session] {
            char name[] = "ffplay";
            std::string input = session->path;
            char* arguments[] = {name, &input[0]};
            session->result = maiFfplayRun(&session->host, 2, arguments);
            session->finished = true;
        });
    } catch (...) {
        delete session;
        return nullptr;
    }
    return session;
}

extern "C" int maiFfplayIosCommand(MaiFfplayIosSession* session,
                                     const char* command) {
    return session && command ? maiFfplaySendCommand(command) : 0;
}

extern "C" int maiFfplayIosSeekPercent(MaiFfplayIosSession* session,
                                         double fraction) {
    return session ? maiFfplaySeekPercent(fraction) : 0;
}

extern "C" int maiFfplayIosHasFinished(const MaiFfplayIosSession* session) {
    return session && session->finished ? 1 : 0;
}

extern "C" int maiFfplayIosExitCode(const MaiFfplayIosSession* session) {
    return session ? session->result.load() : -1;
}

extern "C" void maiFfplayIosStop(MaiFfplayIosSession* session) {
    if (!session) return;
    for (int attempt = 0; attempt < 100 && !session->finished; ++attempt) {
        if (maiFfplaySendCommand("close") == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (session->worker.joinable()) session->worker.join();
    delete session;
}
