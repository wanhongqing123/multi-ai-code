// 文件作用：连接 FFplay 播放内核与 iOS 图像、音频和控制事件的 C 接口。
// 连接 Swift 播放视图与 C++ FFplay 图形／音频引擎。
// 桥接函数负责播放命令、帧呈现和句柄生命周期，不持久化用户媒体。
#include "MaiFfplayIosBridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "../../../../Media/FfplayCompat/MaiFfplayEntry.h"
#include "../../../../Media/MaiGraphicsPresenter.h"

struct MaiFfplayIosSession {
    uint64_t viewId = 0;
    std::string path;
    double speed = 1.0;
    int64_t startPositionUs = 0;
    MaiFfplayHost host{};
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> finished{false};
    std::atomic<int> result{0};
    std::atomic<bool> firstFrameLogged{false};
    std::atomic<uint64_t> presentedFrames{0};
    std::atomic<bool> hlgOutputRequested{false};
    MaiFfplayHdrSourceInfo sourceInfo{};
};

namespace {

bool presentVideo(void* user, const MaiVideoFrame* frame,
                  const MaiVideoSubtitle* subtitle) {
    auto* session = static_cast<MaiFfplayIosSession*>(user);
    const bool hlgFrame = frame && session->sourceInfo.hlg &&
        frame->format == MAI_VIDEO_PIXEL_P010 &&
        frame->color_space == MAI_VIDEO_COLOR_BT2020 &&
        frame->color_transfer == MAI_VIDEO_TRANSFER_HLG;
    if (hlgFrame && !session->hlgOutputRequested.exchange(true)) {
        const uint8_t* ambient = session->sourceInfo.has_ambient
                                     ? session->sourceInfo.ambient : nullptr;
        const uint32_t ambientSize = session->sourceInfo.has_ambient ? 8 : 0;
        if (!maiGraphicsPresenterSetHlgOutput(session->viewId, true, ambient, ambientSize))
            session->hlgOutputRequested = false;
    } else if (!hlgFrame && session->hlgOutputRequested.exchange(false)) {
        maiGraphicsPresenterSetHlgOutput(session->viewId, false, nullptr, 0);
    }
    const bool shown = maiGraphicsPresenterShowVideoFrameWithSubtitle(
        session->viewId, frame, subtitle, false);
    if (!shown) return false;
    const uint64_t count = ++session->presentedFrames;
#if !defined(NDEBUG)
    if (count % 100 == 0)
        std::fprintf(stderr, "[FFplayFrame] presented=%llu\n",
                     static_cast<unsigned long long>(count));
    if (frame && !session->firstFrameLogged.exchange(true)) {
        std::fprintf(stderr, "[FFplayFrame] %ux%u format=%d colorspace=%d range=%d\n",
                     frame->width, frame->height, static_cast<int>(frame->format),
                     static_cast<int>(frame->color_space), static_cast<int>(frame->color_range));
    }
#endif
    return true;
}

bool presentRgba(void* user, const uint8_t* pixels,
                 uint32_t width, uint32_t height, uint32_t stride) {
    auto* session = static_cast<MaiFfplayIosSession*>(user);
    if (session->hlgOutputRequested.exchange(false))
        maiGraphicsPresenterSetHlgOutput(session->viewId, false, nullptr, 0);
    const bool shown = maiGraphicsPresenterShowFrame(session->viewId, pixels,
                                                      width, height, stride, false);
    if (!shown) return false;
    ++session->presentedFrames;
#if !defined(NDEBUG)
    if (!session->firstFrameLogged.exchange(true))
        std::fprintf(stderr, "[FFplayFrame] %ux%u format=RGBA8 stride=%u\n",
                     width, height, stride);
#endif
    return true;
}

}  // namespace

extern "C" MaiFfplayIosSession* maiFfplayIosStartAtSpeed(uint64_t viewId, const char* path,
                                                             double speed,
                                                             int64_t startPositionUs) {
    if (!viewId || !path || !*path) return nullptr;
    auto* session = new MaiFfplayIosSession;
    session->viewId = viewId;
    session->path = path;
    session->speed = speed;
    session->startPositionUs = startPositionUs;
    session->host.user_data = session;
    session->host.graphics_view_id = viewId;
    session->host.present_video = presentVideo;
    session->host.present_rgba = presentRgba;
    try {
        session->worker = std::thread([session] {
            char name[] = "ffplay";
            char nativeRenderer[] = "-enable_vulkan";
            char hardwareDecode[] = "-hwaccel";
            char videoToolbox[] = "videotoolbox";
            std::string input = session->path;
            session->host.present_native_frames = input.find("://") == std::string::npos &&
                maiFfplayProbeHdrSource(input.c_str(), &session->sourceInfo) &&
                session->sourceInfo.native_frame_eligible ? 1 : 0;
            if (session->host.present_native_frames) {
                char* arguments[] = {name, nativeRenderer, hardwareDecode,
                                     videoToolbox, &input[0]};
                session->result = maiFfplayRunAtSpeed(&session->host, 5, arguments,
                                                       session->speed,
                                                       session->startPositionUs);
                // FFplay reports some pre-first-frame filter failures through a
                // quit event whose exit code is zero. The absence of a frame is
                // the reliable signal that this decoder/render path did not open.
                if (!session->stopRequested && session->presentedFrames == 0) {
                    if (session->hlgOutputRequested.exchange(false))
                        maiGraphicsPresenterSetHlgOutput(session->viewId, false, nullptr, 0);
                    session->host.present_native_frames = 0;
                    char* fallback[] = {name, &input[0]};
                    session->result = maiFfplayRunAtSpeed(&session->host, 2, fallback,
                                                           session->speed,
                                                           session->startPositionUs);
                }
            } else {
                char* arguments[] = {name, &input[0]};
                session->result = maiFfplayRunAtSpeed(&session->host, 2, arguments,
                                                       session->speed,
                                                       session->startPositionUs);
            }
            if (!session->stopRequested && session->presentedFrames == 0)
                session->result = -1;
            session->finished = true;
        });
    } catch (...) {
        delete session;
        return nullptr;
    }
    return session;
}

extern "C" MaiFfplayIosSession* maiFfplayIosStart(uint64_t viewId, const char* path) {
    return maiFfplayIosStartAtSpeed(viewId, path, 1.0, 0);
}

extern "C" int maiFfplayIosCommand(MaiFfplayIosSession* session,
                                     const char* command) {
    if (!session || !command) return 0;
    if (std::strcmp(command, "close") == 0) session->stopRequested = true;
    return maiFfplaySendCommand(command);
}

extern "C" int maiFfplayIosSeekPercent(MaiFfplayIosSession* session,
                                         double fraction) {
    return session ? maiFfplaySeekPercent(fraction) : 0;
}

extern "C" int maiFfplayIosSeekAbsoluteUs(MaiFfplayIosSession* session,
                                             int64_t positionUs) {
    return session ? maiFfplaySeekAbsoluteUs(positionUs) : 0;
}

extern "C" int maiFfplayIosHasFinished(const MaiFfplayIosSession* session) {
    return session && session->finished ? 1 : 0;
}

extern "C" int maiFfplayIosExitCode(const MaiFfplayIosSession* session) {
    return session ? session->result.load() : -1;
}

extern "C" void maiFfplayIosStop(MaiFfplayIosSession* session) {
    if (!session) return;
    session->stopRequested = true;
    // A close may land between the hardware attempt and its software retry,
    // when SDL has already cleared the old event queue. Keep asking until the
    // worker exits so the retry cannot outlive its view.
    while (!session->finished) {
        maiFfplaySendCommand("close");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (session->worker.joinable()) session->worker.join();
    if (session->hlgOutputRequested.exchange(false))
        maiGraphicsPresenterSetHlgOutput(session->viewId, false, nullptr, 0);
    delete session;
}
