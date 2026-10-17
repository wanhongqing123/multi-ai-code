// 文件作用：桌面端共用界面；MaiGraphicsMacLayer 负责macOS 图形层与窗口合成，本文件给出其实现。
// 实现边界：与 MaiGraphicsMacLayer 的公开声明配套，调用方通过相应接口进入。
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QWidget>

extern "C" void *maiGraphicsCreateMacLayer(QWidget *widget) {
  widget->setAttribute(Qt::WA_NativeWindow);
  NSView *view = reinterpret_cast<NSView *>(widget->winId());
  [view setWantsLayer:YES];
  CAMetalLayer *layer = [CAMetalLayer layer];
  layer.frame = view.bounds;
  layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
  [view setLayer:layer];
  return [layer retain];
}

extern "C" void maiGraphicsResizeMacLayer(void *nativeLayer, QWidget *widget) {
  if (!nativeLayer || !widget)
    return;
  NSView *view = reinterpret_cast<NSView *>(widget->winId());
  CAMetalLayer *layer = static_cast<CAMetalLayer *>(nativeLayer);
  layer.frame = view.bounds;
  layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
}

extern "C" void maiGraphicsRetainMacLayer(void *nativeLayer) {
  [static_cast<CAMetalLayer *>(nativeLayer) retain];
}

extern "C" void maiGraphicsReleaseMacLayer(void *nativeLayer) {
  [static_cast<CAMetalLayer *>(nativeLayer) release];
}

extern "C" void maiGraphicsStyleMacVideoWindow(QWidget *widget) {
  if (!widget)
    return;
  NSView *view = reinterpret_cast<NSView *>(widget->winId());
  NSWindow *window = view.window;
  if (!window)
    return;
  window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  window.titlebarAppearsTransparent = YES;
  window.backgroundColor = [NSColor colorWithCalibratedRed:0.03 green:0.05 blue:0.08 alpha:1.0];
}

extern "C" void maiGraphicsShareMacVideo(QWidget *anchor, const char *utf8Path) {
  if (!anchor || !utf8Path || !*utf8Path)
    return;
  NSView *view = reinterpret_cast<NSView *>(anchor->winId());
  NSString *path = [NSString stringWithUTF8String:utf8Path];
  if (!view.window || !path)
    return;
  NSURL *url = [NSURL fileURLWithPath:path];
  NSSharingServicePicker *picker = [[NSSharingServicePicker alloc] initWithItems:@[ url ]];
  [picker showRelativeToRect:view.bounds ofView:view preferredEdge:NSRectEdgeMinY];
  [picker release];
}
