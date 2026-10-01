#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "MaiFfplayEntry.h"
#include "MaiGraphicsPresenter.h"

extern "C" uint64_t maiFfplayAppleNonSilentBufferCount(void);

static std::atomic<int> rendered{0};
static std::atomic<bool> sawFiniteSeek{false};
static std::atomic<bool> sawInvalidSeek{false};

static void onLog(void*, int, const char* line) {
    if (!line || !std::strstr(line, "Seek to ") || !std::strstr(line, "%")) return;
    if (std::strstr(line, "inf") || std::strstr(line, "nan")) sawInvalidSeek = true;
    else sawFiniteSeek = true;
}

static void onPresent(uint64_t, bool success, void*) {
    if (success) rendered.fetch_add(1);
}

static bool presentVideo(void* context, const MaiVideoFrame* frame,
                         const MaiVideoSubtitle* subtitle) {
    const uint64_t view = *static_cast<uint64_t*>(context);
    return maiGraphicsPresenterShowVideoFrameWithSubtitle(view, frame, subtitle, false);
}

static bool presentRgba(void* context, const uint8_t* pixels,
                        uint32_t width, uint32_t height, uint32_t stride) {
    const uint64_t view = *static_cast<uint64_t*>(context);
    return maiGraphicsPresenterShowFrame(view, pixels, width, height, stride, false);
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 160, 100)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 160, 100)];
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.frame = view.bounds;
        view.wantsLayer = YES;
        view.layer = layer;
        window.contentView = view;
        [window makeKeyAndOrderFront:nil];

        if (!maiGraphicsPresenterStart(argv[1], argv[2], onPresent, nullptr)) return 3;
        uint64_t viewId = maiGraphicsPresenterAttach((__bridge void*)layer, 160, 100,
                                                      nullptr, nullptr);
        if (!viewId) return 4;
        MaiFfplayHost host{};
        host.user_data = &viewId;
        host.graphics_view_id = viewId;
        host.present_video = presentVideo;
        host.present_rgba = presentRgba;

        for (int attempt = 0; attempt < 2; ++attempt) {
            const int startingFrames = rendered.load();
            const uint64_t startingAudioBuffers = maiFfplayAppleNonSilentBufferCount();
            sawFiniteSeek = false;
            sawInvalidSeek = false;
            maiFfplaySetDiagnosticSink(onLog, nullptr);
            std::atomic<bool> finished{false};
            int result = -1;
            char name[] = "ffplay";
            char loopOption[] = "-loop";
            char infinite[] = "0";
            char* options[] = {name, loopOption, infinite, argv[3]};
            std::thread playback([&] {
                result = maiFfplayRun(&host, 4, options);
                finished = true;
            });
            const auto waitUntil = [&](auto predicate, double seconds) {
                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration<double>(seconds);
                while (!predicate() && !finished && std::chrono::steady_clock::now() < deadline) {
                    [[NSRunLoop currentRunLoop]
                        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
                }
                return predicate();
            };
            const bool started = waitUntil([&] { return rendered.load() > startingFrames; }, 5);
            const bool audibleAudioQueued = started && waitUntil([&] {
                return maiFfplayAppleNonSilentBufferCount() > startingAudioBuffers;
            }, 3);
            MaiFfplayPlaybackStatus status{};
            const bool clockAdvanced = audibleAudioQueued && waitUntil([&] {
                return maiFfplayGetPlaybackStatus(&status) &&
                       status.duration_us > 1'000'000 && status.position_us > 0;
            }, 2);
            const bool pausedCommand = started && maiFfplaySendCommand("pause") == 1;
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
            const int pausedFrames = rendered.load();
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
            const bool paused = pausedCommand && rendered.load() <= pausedFrames + 1;
            const bool resumedCommand = paused && maiFfplaySendCommand("pause") == 1;
            const bool resumed = resumedCommand &&
                waitUntil([&] { return rendered.load() > pausedFrames + 1; }, 2);
            const bool seekAccepted = resumed && maiFfplaySeekPercent(0.5) == 1 &&
                maiFfplaySendCommand("not_a_command") == -1;
            const bool seekHandled = seekAccepted &&
                waitUntil([&] { return sawFiniteSeek.load(); }, 2) && !sawInvalidSeek;
            if (!finished) maiFfplayRequestQuit();
            playback.join();
            maiFfplaySetDiagnosticSink(nullptr, nullptr);
            if (result != 0 || !started || !audibleAudioQueued || !clockAdvanced ||
                !paused || !resumed || !seekHandled) return 6;
        }
        maiGraphicsPresenterDetach(viewId);
        maiGraphicsPresenterStop();
        return 0;
    }
}
