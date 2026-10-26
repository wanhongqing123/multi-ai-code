// Regression for an HDR video whose native FFplay path exits before its first frame.
// Build with MaiFfplayIosBridge.cpp and no real FFmpeg or graphics dependency.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../ios/MaiChat/MaiChat/Media/MaiFfplayIosBridge.h"
#include "../FfplayCompat/MaiFfplayEntry.h"

namespace {

enum class Scenario { NativeZeroFrames, NativeSuccess, NativePresenterRejected,
                      SoftwareRgbaOnly, BothZeroFrames, StopNative };
std::atomic<Scenario> scenario{Scenario::NativeZeroFrames};
std::atomic<int> nativeRuns{0};
std::atomic<int> softwareRuns{0};
std::atomic<int> closeRequests{0};
std::atomic<bool> allowPresentation{true};

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
}

void reportFrame(const MaiFfplayHost* host) {
    MaiVideoFrame frame{};
    frame.width = 1;
    frame.height = 1;
    host->present_video(host->user_data, &frame, nullptr);
}

int failed(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

}  // namespace

extern "C" int maiFfplaySourceHasHdrWithoutSubtitles(const char*) {
    return scenario == Scenario::SoftwareRgbaOnly ? 0 : 1;
}

extern "C" int maiFfplayRunAtSpeed(const MaiFfplayHost* host, int argc, char**,
                                    double, int64_t) {
    if (argc == 5) {
        ++nativeRuns;
        if (scenario == Scenario::NativeSuccess) reportFrame(host);
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
    maiFfplayIosStop(session);

    reset(Scenario::SoftwareRgbaOnly);
    session = maiFfplayIosStart(1, "/tmp/software-rgba-video.mp4");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("ordinary software RGBA playback did not finish");
    if (nativeRuns != 0 || softwareRuns != 1 || maiFfplayIosExitCode(session) != 0)
        return failed("presented RGBA frame must count as successful playback");
    maiFfplayIosStop(session);

    reset(Scenario::NativeSuccess);
    session = maiFfplayIosStart(1, "/tmp/ordinary-hlg.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("native success did not finish");
    if (nativeRuns != 1 || softwareRuns != 0 || maiFfplayIosExitCode(session) != 0)
        return failed("native success should not retry");
    maiFfplayIosStop(session);

    reset(Scenario::BothZeroFrames);
    session = maiFfplayIosStart(1, "/tmp/unplayable.mov");
    if (!session || !waitFor([&] { return maiFfplayIosHasFinished(session) == 1; }))
        return failed("both zero-frame attempts did not finish");
    if (nativeRuns != 1 || softwareRuns != 1 || maiFfplayIosExitCode(session) == 0)
        return failed("two zero-frame attempts must be reported as failure");
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
