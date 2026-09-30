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
