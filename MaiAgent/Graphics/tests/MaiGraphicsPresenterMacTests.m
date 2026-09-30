#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdatomic.h>
#include <stdio.h>

#include "MaiGraphicsPresenter.h"

static atomic_int present_result;

static void onPresent(uint64_t view_id, bool success, void* user_data) {
    (void)view_id;
    (void)user_data;
    atomic_store(&present_result, success ? 1 : -1);
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
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
        if (id) maiGraphicsPresenterDetach(id);
        maiGraphicsPresenterStop();
        [window close];
        if (!passed) fputs("Direct Metal presentation failed\n", stderr);
        return passed ? 0 : 1;
    }
}
