#include <QByteArray>
#include <QString>

#import <AppKit/AppKit.h>
#import <PDFKit/PDFKit.h>

@class MaiPdfWindowKeeper;
static NSMutableSet<MaiPdfWindowKeeper *> *activePdfWindows;

@interface MaiPdfWindowKeeper : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@end

@implementation MaiPdfWindowKeeper
- (void)windowWillClose:(NSNotification *)notification {
  (void)notification;
  MaiPdfWindowKeeper *keeper = self;
  self.window.delegate = nil;
  [activePdfWindows removeObject:keeper];
}
@end

bool showMacPdfPreview(const QString &path, const QString &displayName) {
  @autoreleasepool {
    const QByteArray utf8 = path.toUtf8();
    NSString *name = [NSString stringWithUTF8String:utf8.constData()];
    if (name == nil)
      return false;
    NSURL *url = [NSURL fileURLWithPath:name];
    PDFDocument *document = [[PDFDocument alloc] initWithURL:url];
    if (document == nil || document.pageCount == 0)
      return false;

    const NSRect frame = NSMakeRect(0, 0, 1000, 760);
    NSWindow *window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled |
                                              NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskMiniaturizable |
                                              NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.releasedWhenClosed = NO;
    const QByteArray titleUtf8 = displayName.toUtf8();
    NSString *title =
        displayName.isEmpty()
            ? name.lastPathComponent
            : [NSString stringWithUTF8String:titleUtf8.constData()];
    window.title = title;
    PDFView *viewer = [[PDFView alloc] initWithFrame:frame];
    viewer.document = document;
    viewer.autoScales = YES;
    viewer.displayMode = kPDFDisplaySinglePageContinuous;
    window.contentView = viewer;

    if (activePdfWindows == nil)
      activePdfWindows = [NSMutableSet new];
    MaiPdfWindowKeeper *keeper = [MaiPdfWindowKeeper new];
    keeper.window = window;
    window.delegate = keeper;
    [activePdfWindows addObject:keeper];
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return true;
  }
}
