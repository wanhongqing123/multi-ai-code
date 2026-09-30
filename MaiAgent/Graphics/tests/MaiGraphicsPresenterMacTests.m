#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdatomic.h>
#include <stdio.h>

#include "MaiGraphicsPresenter.h"
#include "MaiVideoPlayback.h"

static atomic_int present_result;
static atomic_int presented_frames;
static atomic_int video_status;

static void onPresent(uint64_t view_id, bool success, void* user_data) {
    (void)view_id;
    (void)user_data;
    atomic_store(&present_result, success ? 1 : -1);
    if (success) atomic_fetch_add(&presented_frames, 1);
}

static void onVideo(MaiVideoPlayback* playback, MaiVideoPlaybackEvent event,
                    void* user_data) {
    (void)playback;
    (void)user_data;
    if (event == MAI_VIDEO_READY) atomic_store(&video_status, 1);
    if (event == MAI_VIDEO_ERROR) atomic_store(&video_status, -1);
}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 64, 64)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.frame = view.bounds;
        view.wantsLayer = YES;
        view.layer = layer;
        window.contentView = view;
        [window makeKeyAndOrderFront:nil];

        bool passed = maiGraphicsPresenterStart(argv[1], argv[2], onPresent, NULL);
        uint64_t id = passed ? maiGraphicsPresenterAttach((__bridge void*)layer, 64, 64,
                                                            NULL, NULL) : 0;
        passed = id != 0 && maiGraphicsPresenterShowImage(id, argv[3], false);
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
        while (passed && atomic_load(&present_result) == 0 &&
               [deadline timeIntervalSinceNow] > 0.0) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        passed = passed && atomic_load(&present_result) == 1;
        if (passed) {
            const uint8_t green[] = {0, 255, 0, 255, 0, 255, 0, 255,
                                     0, 255, 0, 255, 0, 255, 0, 255};
            atomic_store(&present_result, 0);
            passed = maiGraphicsPresenterShowFrame(id, green, 2, 2, 8, false);
            NSDate* frameDeadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
            while (passed && atomic_load(&present_result) == 0 &&
                   [frameDeadline timeIntervalSinceNow] > 0.0) {
                [[NSRunLoop currentRunLoop]
                    runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            }
            passed = passed && atomic_load(&present_result) == 1;
        }
        if (passed) {
            MaiVideoPlayback* video = maiVideoPlaybackCreate(id, argv[4], onVideo, NULL);
            passed = video != NULL;
            NSDate* readyDeadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
            while (passed && atomic_load(&video_status) == 0 &&
                   [readyDeadline timeIntervalSinceNow] > 0.0)
                [[NSRunLoop currentRunLoop]
                    runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            passed = passed && atomic_load(&video_status) == 1;
            const int firstFrameCount = atomic_load(&presented_frames);
            if (passed) passed = maiVideoPlaybackPlay(video);
            NSDate* playingDeadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
            while (passed && atomic_load(&presented_frames) < firstFrameCount + 2 &&
                   [playingDeadline timeIntervalSinceNow] > 0.0)
                [[NSRunLoop currentRunLoop]
                    runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            passed = passed && atomic_load(&presented_frames) >= firstFrameCount + 2;
            if (passed) passed = maiVideoPlaybackPause(video);
            if (passed) passed = maiVideoPlaybackSeek(video, 1000);
            if (passed) passed = maiVideoPlaybackStep(video);
            const int pausedFrameCount = atomic_load(&presented_frames);
            NSDate* stepDeadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
            while (passed && atomic_load(&presented_frames) <= pausedFrameCount &&
                   [stepDeadline timeIntervalSinceNow] > 0.0)
                [[NSRunLoop currentRunLoop]
                    runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            passed = passed && atomic_load(&presented_frames) > pausedFrameCount;
            maiVideoPlaybackDestroy(video);
        }
        if (id) maiGraphicsPresenterDetach(id);
        maiGraphicsPresenterStop();
        [window close];
        if (!passed) fputs("Direct Metal presentation failed\n", stderr);
        return passed ? 0 : 1;
    }
}
