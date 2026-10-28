// Regression for an HDR video whose native FFplay path exits before its first frame.
// Build with MaiFfplayIosBridge.cpp and no real FFmpeg or graphics dependency.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../ios/MaiChat/MaiChat/Media/MaiFfplayIosBridge.h"
#include "../FfplayCompat/MaiFfplayEntry.h"
#include "../../Network/MaiCloudVod.h"

namespace {

enum class Scenario { NativeZeroFrames, NativeSuccess, NativeThenRgba,
                      NativePresenterRejected,
                      SoftwareRgbaOnly, BothZeroFrames, StopNative };
std::atomic<Scenario> scenario{Scenario::NativeZeroFrames};
std::atomic<int> nativeRuns{0};
std::atomic<int> softwareRuns{0};
std::atomic<int> closeRequests{0};
std::atomic<bool> allowPresentation{true};
std::atomic<int> hlgEnableRequests{0};
std::atomic<int> hlgDisableRequests{0};

template <class Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return predicate();
}

void reset(Scenario selected) {
    scenario = selected;
    nativeRuns = 0;
    softwareRuns = 0;
    closeRequests = 0;
    allowPresentation = true;
    hlgEnableRequests = 0;
    hlgDisableRequests = 0;
}

void reportFrame(const MaiFfplayHost* host) {
    MaiVideoFrame frame{};
    frame.width = 1;
    frame.height = 1;
    if (host->present_native_frames) {
        frame.format = MAI_VIDEO_PIXEL_P010;
        frame.color_space = MAI_VIDEO_COLOR_BT2020;
        frame.color_transfer = MAI_VIDEO_TRANSFER_HLG;
    } else {
        frame.format = MAI_VIDEO_PIXEL_I420;
        frame.color_space = MAI_VIDEO_COLOR_BT709;
    }
    host->present_video(host->user_data, &frame, nullptr);
}

int failed(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

}  // namespace

extern "C" int maiFfplayProbeHdrSource(const char*, MaiFfplayHdrSourceInfo* info) {
    *info = {};
    info->native_frame_eligible = scenario == Scenario::SoftwareRgbaOnly ? 0 : 1;
    info->hlg = info->native_frame_eligible;
    info->has_ambient = info->hlg;
    const uint8_t ambient[] = {0, 47, 233, 160, 61, 19, 64, 66};
    std::memcpy(info->ambient, ambient, sizeof(ambient));
    return 1;
}

// These tests use local media only. Keep unrelated cloud entry points inert so
// the same bridge can be linked without a network stack in this regression.
extern "C" int maiFfplayProbeHdrSourceFromHost(const MaiFfplayHost*, MaiFfplayHdrSourceInfo*) { return 0; }
extern "C" uint64_t mai_cloud_vod_size(const MaiCloudVod*) { return 0; }
extern "C" void mai_cloud_vod_interrupt_read(MaiCloudVod*) {}
extern "C" int mai_cloud_vod_read_at(MaiCloudVod*, uint64_t, uint8_t*, size_t,
                                     size_t*, MaiCloudVodResult*) { return 0; }

extern "C" int maiFfplayRunAtSpeed(const MaiFfplayHost* host, int argc, char**,
                                    double, int64_t) {
    if (argc == 5) {
        ++nativeRuns;
        if (scenario == Scenario::NativeSuccess) reportFrame(host);
        if (scenario == Scenario::NativeThenRgba) {
            reportFrame(host);
            const uint8_t pixel[] = {0, 0, 0, 255};
            host->present_rgba(host->user_data, pixel, 1, 1, 4);
        }
        if (scenario == Scenario::NativePresenterRejected) {
            allowPresentation = false;
            reportFrame(host);
        }
        if (scenario == Scenario::StopNative) {
            while (closeRequests == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 0;
    }
    ++softwareRuns;
    if (scenario == Scenario::NativePresenterRejected) allowPresentation = true;
    if (scenario == Scenario::SoftwareRgbaOnly) {
        const uint8_t pixel[] = {0, 0, 0, 255};
        host->present_rgba(host->user_data, pixel, 1, 1, 4);
    } else if (scenario != Scenario::BothZeroFrames) reportFrame(host);
    return 0;
}

extern "C" int maiFfplaySendCommand(const char* command) {
    if (command && std::strcmp(command, "close") == 0) ++closeRequests;
    return 1;
}

extern "C" int maiFfplaySeekPercent(double) { return 1; }
extern "C" int maiFfplaySeekAbsoluteUs(int64_t) { return 1; }
extern "C" bool maiGraphicsPresenterShowVideoFrameWithSubtitle(
    uint64_t, const MaiVideoFrame*, const MaiVideoSubtitle*, bool) { return allowPresentation; }
extern "C" bool maiGraphicsPresenterShowFrame(
    uint64_t, const uint8_t*, uint32_t, uint32_t, uint32_t, bool) { return allowPresentation; }
extern "C" bool maiGraphicsPresenterSetHlgOutput(uint64_t, bool enabled,
                                                   const uint8_t* ambient,
                                                   uint32_t ambientSize) {
    if (enabled) {
        if (!ambient || ambientSize != 8 || ambient[0] != 0 || ambient[1] != 47 ||
            ambient[2] != 233 || ambient[3] != 160)
            return false;
        ++hlgEnableRequests;
    } else {
        ++hlgDisableRequests;
    }
    return true;
}

int main() {
    reset(Scenario::NativeZeroFrames);
    auto* session = maiFfplayIosStart(1, "/tmp/rotated-hlg.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("zero-frame native attempt did not finish");
    if (nativeRuns != 1 || softwareRuns != 1 || maiFfplayIosExitCode(session) != 0)
        return failed("zero-frame native attempt did not fall back to software");
    maiFfplayIosStop(session);

    reset(Scenario::NativePresenterRejected);
    session = maiFfplayIosStart(1, "/tmp/native-presenter-rejected.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("rejected native presentation did not finish");
    if (nativeRuns != 1 || softwareRuns != 1 || maiFfplayIosExitCode(session) != 0)
        return failed("a rejected native frame must not suppress software fallback");
    if (hlgEnableRequests != 1 || hlgDisableRequests != 1)
        return failed("rejected HLG frame did not restore SDR before software fallback");
    maiFfplayIosStop(session);

    reset(Scenario::SoftwareRgbaOnly);
    session = maiFfplayIosStart(1, "/tmp/software-rgba-video.mp4");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("ordinary software RGBA playback did not finish");
    if (nativeRuns != 0 || softwareRuns != 1 || maiFfplayIosExitCode(session) != 0)
        return failed("presented RGBA frame must count as successful playback");
    if (maiFfplayIosHasFirstFrame(session) != 1)
        return failed("presented RGBA frame was not exposed to the viewer");
    if (hlgEnableRequests != 0 || hlgDisableRequests != 0)
        return failed("SDR playback must not change the Metal layer color mode");
    maiFfplayIosStop(session);

    reset(Scenario::NativeSuccess);
    session = maiFfplayIosStart(1, "/tmp/ordinary-hlg.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("native success did not finish");
    if (nativeRuns != 1 || softwareRuns != 0 || maiFfplayIosExitCode(session) != 0)
        return failed("native success should not retry");
    if (maiFfplayIosHasFirstFrame(session) != 1)
        return failed("presented native frame was not exposed to the viewer");
    if (hlgEnableRequests != 1 || hlgDisableRequests != 0)
        return failed("native HLG playback did not request per-view EDR");
    maiFfplayIosStop(session);
    if (hlgDisableRequests != 1)
        return failed("closed HLG session did not return its view to SDR");

    scenario = Scenario::SoftwareRgbaOnly;
    session = maiFfplayIosStart(1, "/tmp/sdr-after-hlg.mp4");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("SDR session after HLG did not finish");
    maiFfplayIosStop(session);
    if (hlgEnableRequests != 1 || hlgDisableRequests != 1)
        return failed("SDR session after HLG changed the color mode again");

    reset(Scenario::NativeThenRgba);
    session = maiFfplayIosStart(1, "/tmp/native-then-rgba.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("native-to-RGBA transition did not finish");
    if (hlgEnableRequests != 1 || hlgDisableRequests != 1)
        return failed("RGBA frame did not restore SDR before presentation");
    maiFfplayIosStop(session);
    if (hlgDisableRequests != 1)
        return failed("closing an already-SDR session reset its view twice");

    reset(Scenario::BothZeroFrames);
    session = maiFfplayIosStart(1, "/tmp/unplayable.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("both zero-frame attempts did not finish");
    if (nativeRuns != 1 || softwareRuns != 1 || maiFfplayIosExitCode(session) == 0)
        return failed("two zero-frame attempts must be reported as failure");
    if (maiFfplayIosHasFirstFrame(session) != 0 || maiFfplayIosHasFirstFrame(nullptr) != 0)
        return failed("zero-frame or absent session was reported as a ready video");
    maiFfplayIosStop(session);

    reset(Scenario::StopNative);
    session = maiFfplayIosStart(1, "/tmp/closing.mov");
    if (!session || !waitFor([] { return nativeRuns == 1; }))
        return failed("native attempt did not begin before close");
    maiFfplayIosStop(session);
    if (softwareRuns != 0 || closeRequests == 0)
        return failed("explicit close started an unwanted software retry");

    reset(Scenario::StopNative);
    session = maiFfplayIosStart(1, "/tmp/agent-closing.mov");
    if (!session || !waitFor([] { return nativeRuns == 1; }))
        return failed("native attempt did not begin before Agent close");
    if (maiFfplayIosCommand(session, "close") != 1 ||
        !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("Agent close did not finish the native attempt");
    if (softwareRuns != 0) return failed("Agent close started an unwanted software retry");
    maiFfplayIosStop(session);

    return 0;
}
