#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "MaiFfplayEntry.h"
#include "MaiGraphicsPresenter.h"

static std::atomic<int> rendered{0};

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
            std::atomic<bool> finished{false};
            int result = -1;
            bool controlsSent = false;
            char name[] = "ffplay";
            char autoexit[] = "-autoexit";
            char volume[] = "-volume";
            char muted[] = "0";
            char* options[] = {name, autoexit, volume, muted, argv[3]};
            std::thread playback([&] {
                result = maiFfplayRun(&host, 5, options);
                finished = true;
            });
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(12);
            while (!finished && std::chrono::steady_clock::now() < deadline) {
                if (!controlsSent && rendered.load() > startingFrames) {
                    controlsSent = maiFfplaySendCommand("pause") == 1 &&
                                   maiFfplaySendCommand("pause") == 1 &&
                                   maiFfplaySeekPercent(0.5) == 1 &&
                                   maiFfplaySendCommand("not_a_command") == -1;
                }
                [[NSRunLoop currentRunLoop]
                    runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            }
            if (!finished) {
                maiFfplayRequestQuit();
            }
            playback.join();
            if (result != 0 || rendered.load() <= startingFrames || !controlsSent) return 6;
        }
        maiGraphicsPresenterDetach(viewId);
        maiGraphicsPresenterStop();
        return 0;
    }
}
