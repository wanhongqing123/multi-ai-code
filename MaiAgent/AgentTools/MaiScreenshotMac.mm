#include "MaiScreenshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "MaiBlockingCheck.h"

namespace {

constexpr double kMaximumWidth = 2560.0;
constexpr double kMaximumHeight = 1600.0;
constexpr auto kCaptureTimeout = std::chrono::seconds(20);

struct MaiCaptureState {
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    MaiError error;
    MaiScreenshot screenshot;
    std::vector<std::string> titles;
};

std::string errorText(NSError* error, const char* fallback) {
    if (!error) return fallback;
    const char* text = error.localizedDescription.UTF8String;
    return text && *text ? std::string(text) : std::string(fallback);
}

void finishCapture(const std::shared_ptr<MaiCaptureState>& state, MaiError error,
                   MaiScreenshot screenshot = {}) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->finished) return;
        state->error = std::move(error);
        state->screenshot = std::move(screenshot);
        state->finished = true;
    }
    state->condition.notify_one();
}

MaiError waitForCapture(const std::shared_ptr<MaiCaptureState>& state, MaiScreenshot& screenshot) {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, kCaptureTimeout, [&] { return state->finished; })) {
        return MaiError::make(MaiErrorCode::Internal, "screen capture timed out");
    }
    if (state->error.hasError()) return state->error;
    screenshot = std::move(state->screenshot);
    return {};
}

MaiError encodePng(CGImageRef image, MaiScreenshot& screenshot) {
    if (!image || !CGImageGetWidth(image) || !CGImageGetHeight(image)) {
        return MaiError::make(MaiErrorCode::Internal, "screen capture returned no image");
    }
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    if (!data) return MaiError::make(MaiErrorCode::Internal, "cannot allocate screenshot data");
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr);
    if (!destination) {
        CFRelease(data);
        return MaiError::make(MaiErrorCode::Internal, "cannot create PNG encoder");
    }
    CGImageDestinationAddImage(destination, image, nullptr);
    const bool saved = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    const CFIndex length = CFDataGetLength(data);
    if (!saved || length <= 0 || length > 24 * 1024 * 1024) {
        CFRelease(data);
        return MaiError::make(MaiErrorCode::Internal, "screen capture PNG is empty or too large");
    }
    screenshot.pngBytes.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + length);
    screenshot.width = static_cast<int>(CGImageGetWidth(image));
    screenshot.height = static_cast<int>(CGImageGetHeight(image));
    CFRelease(data);
    return {};
}

SCDisplay* displayUnderPointer(SCShareableContent* content) {
    CGEventRef event = CGEventCreate(nullptr);
    const CGPoint pointer = event ? CGEventGetLocation(event) : CGPointZero;
    if (event) CFRelease(event);
    for (SCDisplay* display in content.displays) {
        if (CGRectContainsPoint(CGDisplayBounds(display.displayID), pointer)) return display;
    }
    for (SCDisplay* display in content.displays) {
        if (display.displayID == CGMainDisplayID()) return display;
    }
    return content.displays.firstObject;
}

SCWindow* windowWithTitle(SCShareableContent* content, const std::string& utf8, MaiError& error) {
    NSString* requested = [[NSString alloc] initWithBytes:utf8.data()
                                                   length:utf8.size()
                                                 encoding:NSUTF8StringEncoding];
    if (!requested || requested.length == 0) {
        error = MaiError::make(MaiErrorCode::InvalidInput,
                               "windowTitle is empty or is not valid UTF-8");
        return nil;
    }
    NSMutableArray<SCWindow*>* exact = [NSMutableArray array];
    NSMutableArray<SCWindow*>* partial = [NSMutableArray array];
    for (SCWindow* window in content.windows) {
        if (!window.isOnScreen || window.windowLayer != 0 || window.title.length == 0) continue;
        if ([window.title caseInsensitiveCompare:requested] == NSOrderedSame) {
            [exact addObject:window];
        } else if ([window.title rangeOfString:requested options:NSCaseInsensitiveSearch]
                       .location != NSNotFound) {
            [partial addObject:window];
        }
    }
    NSArray<SCWindow*>* matches = exact.count ? exact : partial;
    if (matches.count == 0) {
        error = MaiError::make(MaiErrorCode::NotFound,
                               "no visible top-level window matches windowTitle");
        return nil;
    }
    if (matches.count != 1) {
        error = MaiError::make(MaiErrorCode::InvalidInput,
                               "windowTitle matches more than one visible window");
        return nil;
    }
    return matches.firstObject;
}

SCStreamConfiguration* screenshotConfiguration(SCContentFilter* filter) {
    const double scale = std::max(1.0, double(filter.pointPixelScale));
    const double width = CGRectGetWidth(filter.contentRect) * scale;
    const double height = CGRectGetHeight(filter.contentRect) * scale;
    if (width <= 0 || height <= 0) return nil;
    const double factor = std::min({1.0, kMaximumWidth / width, kMaximumHeight / height});
    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    configuration.width = static_cast<size_t>(std::max(1.0, std::round(width * factor)));
    configuration.height = static_cast<size_t>(std::max(1.0, std::round(height * factor)));
    configuration.showsCursor = YES;
    return configuration;
}

void requestShareableContent(const std::shared_ptr<MaiCaptureState>& state,
                             MaiScreenshotTarget target, std::string title) {
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                   @autoreleasepool {
                                       if (!content) {
                                           finishCapture(
                                               state,
                                               MaiError::make(
                                                   MaiErrorCode::Internal,
                                                   errorText(error,
                                                             "screen content is unavailable")));
                                           return;
                                       }
                                       SCContentFilter* filter = nil;
                                       if (target == MaiScreenshotTarget::WindowByTitle) {
                                           MaiError lookupError;
                                           SCWindow* window =
                                               windowWithTitle(content, title, lookupError);
                                           if (!window) {
                                               finishCapture(state, std::move(lookupError));
                                               return;
                                           }
                                           filter = [[SCContentFilter alloc]
                                               initWithDesktopIndependentWindow:window];
                                       } else {
                                           SCDisplay* display = displayUnderPointer(content);
                                           if (!display) {
                                               finishCapture(state,
                                                             MaiError::make(
                                                                 MaiErrorCode::NotFound,
                                                                 "no active display is available"));
                                               return;
                                           }
                                           filter = [[SCContentFilter alloc] initWithDisplay:display
                                                                            excludingWindows:@[]];
                                       }
                                       SCStreamConfiguration* configuration =
                                           screenshotConfiguration(filter);
                                       if (!configuration) {
                                           finishCapture(
                                               state, MaiError::make(
                                                          MaiErrorCode::Internal,
                                                          "screen capture dimensions are invalid"));
                                           return;
                                       }
                                       [SCScreenshotManager
                                           captureImageWithFilter:filter
                                                    configuration:configuration
                                                completionHandler:^(CGImageRef image,
                                                                    NSError* captureError) {
                                                  @autoreleasepool {
                                                      MaiScreenshot result;
                                                      MaiError converted =
                                                          captureError
                                                              ? MaiError::make(
                                                                    MaiErrorCode::Internal,
                                                                    errorText(
                                                                        captureError,
                                                                        "screen capture failed"))
                                                              : encodePng(image, result);
                                                      finishCapture(state, std::move(converted),
                                                                    std::move(result));
                                                  }
                                                }];
                                   }
                                 }];
}

}  // namespace

bool maiIsScreenshotSupported() {
    if (@available(macOS 14.0, *)) return true;
    return false;
}

MaiError maiCaptureScreenshot(const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
    maiAssertBlockingAllowed("maiCaptureScreenshot");
    screenshot = MaiScreenshot{};
    if (!maiIsScreenshotSupported()) {
        return MaiError::make(MaiErrorCode::NotSupported,
                              "ScreenCaptureKit requires macOS 14 or later");
    }
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
        return MaiError::make(
            MaiErrorCode::NotConfigured,
            "Allow Screen & System Audio Recording for MaiChat, restart it, and retry");
    }
    const auto state = std::make_shared<MaiCaptureState>();
    requestShareableContent(state, request.target, request.windowTitle);
    return waitForCapture(state, screenshot);
}

MaiError maiListCaptureWindows(std::vector<std::string>& titles) {
    maiAssertBlockingAllowed("maiListCaptureWindows");
    titles.clear();
    if (!maiIsScreenshotSupported()) {
        return MaiError::make(MaiErrorCode::NotSupported,
                              "ScreenCaptureKit requires macOS 14 or later");
    }
    if (!CGPreflightScreenCaptureAccess()) {
        return MaiError::make(MaiErrorCode::NotConfigured,
                              "Allow Screen & System Audio Recording for MaiChat and restart it");
    }
    const auto state = std::make_shared<MaiCaptureState>();
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                   @autoreleasepool {
                                       if (!content) {
                                           finishCapture(
                                               state,
                                               MaiError::make(
                                                   MaiErrorCode::Internal,
                                                   errorText(error,
                                                             "screen content is unavailable")));
                                           return;
                                       }
                                       std::vector<std::string> found;
                                       for (SCWindow* window in content.windows) {
                                           if (!window.isOnScreen || window.windowLayer != 0 ||
                                               window.title.length == 0)
                                               continue;
                                           const char* utf8 = window.title.UTF8String;
                                           if (utf8 && *utf8) found.emplace_back(utf8);
                                       }
                                       std::sort(found.begin(), found.end());
                                       {
                                           std::lock_guard<std::mutex> guard(state->mutex);
                                           state->titles = std::move(found);
                                           state->finished = true;
                                       }
                                       state->condition.notify_one();
                                   }
                                 }];
    MaiScreenshot unused;
    const MaiError result = waitForCapture(state, unused);
    if (result.hasError()) return result;
    titles = std::move(state->titles);
    return {};
}
