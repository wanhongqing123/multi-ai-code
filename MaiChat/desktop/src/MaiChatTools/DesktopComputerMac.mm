#include "MaiChatTools/DesktopComputerPlatform.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include <QString>

namespace {

std::string utf8(CFStringRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    const CFIndex length = CFStringGetLength(value);
    if (length > 4096) return {};
    std::vector<char> bytes(std::size_t(length * 4 + 1), 0);
    if (!CFStringGetCString(value, bytes.data(), CFIndex(bytes.size()),
                            kCFStringEncodingUTF8)) return {};
    return bytes.data();
}

std::string attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess ||
        !value) return {};
    const std::string result = utf8(static_cast<CFStringRef>(value));
    CFRelease(value);
    return result;
}

bool bounds(AXUIElementRef element, DesktopNativeElement& result) {
    CFTypeRef position = nullptr;
    CFTypeRef size = nullptr;
    const bool found = AXUIElementCopyAttributeValue(element, kAXPositionAttribute,
        &position) == kAXErrorSuccess && position &&
        AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size) == kAXErrorSuccess && size;
    CGPoint point{};
    CGSize dimensions{};
    const bool valid = found && CFGetTypeID(position) == AXValueGetTypeID() &&
        CFGetTypeID(size) == AXValueGetTypeID() &&
        AXValueGetValue(static_cast<AXValueRef>(position),
                        static_cast<AXValueType>(kAXValueCGPointType), &point) &&
        AXValueGetValue(static_cast<AXValueRef>(size),
                        static_cast<AXValueType>(kAXValueCGSizeType), &dimensions);
    if (position) CFRelease(position);
    if (size) CFRelease(size);
    if (!valid) return false;
    result.x = point.x;
    result.y = point.y;
    result.width = dimensions.width;
    result.height = dimensions.height;
    return true;
}

AXUIElementRef accessibilityWindow(const DesktopNativeWindow& window) {
    AXUIElementRef application = AXUIElementCreateApplication(pid_t(window.processId));
    if (!application) return nullptr;
    CFTypeRef value = nullptr;
    AXUIElementRef found = nullptr;
    if (AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &value) ==
            kAXErrorSuccess && value && CFGetTypeID(value) == CFArrayGetTypeID()) {
        const CFArrayRef windows = static_cast<CFArrayRef>(value);
        for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
            AXUIElementRef candidate = static_cast<AXUIElementRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(windows, index)));
            if (CFGetTypeID(candidate) == AXUIElementGetTypeID() &&
                attribute(candidate, kAXTitleAttribute) == window.title) {
                found = candidate;
                CFRetain(found);
                break;
            }
        }
    }
    if (value) CFRelease(value);
    CFRelease(application);
    return found;
}

MaiError accessPermission() {
    if (!AXIsProcessTrusted())
        return {MaiErrorCode::NotConfigured,
                "grant MaiChat Accessibility permission in macOS Settings before computer actions"};
    return {};
}

MaiError postKey(CGKeyCode key) {
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, key, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, key, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return {MaiErrorCode::Internal, "could not create keyboard event"};
    }
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    return {};
}

} // namespace

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows() {
    if (!CGPreflightScreenCaptureAccess())
        return {MaiErrorCode::NotConfigured,
                "grant MaiChat Screen & System Audio Recording permission and restart it"};
    std::vector<DesktopNativeWindow> output;
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!windows) return {MaiErrorCode::NotSupported, "could not list visible macOS windows"};
    for (CFIndex index = 0; index < CFArrayGetCount(windows) && output.size() < 200; ++index) {
        CFDictionaryRef details = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(windows, index));
        if (!details || CFGetTypeID(details) != CFDictionaryGetTypeID()) continue;
        const auto name = static_cast<CFStringRef>(
            CFDictionaryGetValue(details, kCGWindowName));
        const std::string title = utf8(name);
        if (title.empty()) continue;
        int layer = -1;
        const auto layerValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(details, kCGWindowLayer));
        if (!layerValue || !CFNumberGetValue(layerValue, kCFNumberIntType, &layer) ||
            layer != 0) continue;
        CGRect frame{};
        const auto frameValue = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(details, kCGWindowBounds));
        if (!frameValue || !CGRectMakeWithDictionaryRepresentation(frameValue, &frame) ||
            frame.size.width < 80 || frame.size.height < 50) continue;
        std::uint32_t id = 0;
        int pid = 0;
        const auto idValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(details, kCGWindowNumber));
        const auto pidValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(details, kCGWindowOwnerPID));
        if (!idValue || !pidValue ||
            !CFNumberGetValue(idValue, kCFNumberSInt32Type, &id) ||
            !CFNumberGetValue(pidValue, kCFNumberIntType, &pid)) continue;
        DesktopNativeWindow window;
        window.id = id;
        window.processId = std::uint64_t(pid);
        window.title = title;
        window.application = utf8(static_cast<CFStringRef>(
            CFDictionaryGetValue(details, kCGWindowOwnerName)));
        window.x = frame.origin.x;
        window.y = frame.origin.y;
        window.width = frame.size.width;
        window.height = frame.size.height;
        output.push_back(std::move(window));
    }
    CFRelease(windows);
    return output;
}

MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(
    const DesktopNativeWindow& window) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    AXUIElementRef root = accessibilityWindow(window);
    if (!root) return {MaiErrorCode::NotFound, "window is unavailable to macOS Accessibility"};
    std::vector<DesktopNativeElement> output;
    std::vector<AXUIElementRef> pending{root};
    for (std::size_t index = 0; index < pending.size() && index < 250; ++index) {
        AXUIElementRef current = pending[index];
        DesktopNativeElement element;
        element.role = attribute(current, kAXRoleAttribute);
        element.label = attribute(current, kAXTitleAttribute);
        if (element.label.empty())
            element.label = attribute(current, kAXDescriptionAttribute);
        if (element.label.empty())
            element.label = attribute(current, kAXPlaceholderValueAttribute);
        if (!element.role.empty() && !element.label.empty() && bounds(current, element) &&
            element.width > 0 && element.height > 0 && output.size() < 80)
            output.push_back(std::move(element));
        CFTypeRef children = nullptr;
        if (AXUIElementCopyAttributeValue(current, kAXChildrenAttribute, &children) ==
                kAXErrorSuccess && children && CFGetTypeID(children) == CFArrayGetTypeID()) {
            const CFArrayRef array = static_cast<CFArrayRef>(children);
            for (CFIndex child = 0; child < CFArrayGetCount(array) && pending.size() < 250;
                 ++child) {
                AXUIElementRef value = static_cast<AXUIElementRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(array, child)));
                if (CFGetTypeID(value) == AXUIElementGetTypeID()) {
                    CFRetain(value);
                    pending.push_back(value);
                }
            }
        }
        if (children) CFRelease(children);
        CFRelease(current);
    }
    // Any retained children after the traversal cap still belong to this call.
    for (std::size_t index = std::min<std::size_t>(pending.size(), 250);
         index < pending.size(); ++index) CFRelease(pending[index]);
    return output;
}

MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    __block bool activated = false;
    const auto work = ^{
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication
                runningApplicationWithProcessIdentifier:pid_t(window.processId)];
            activated = app && [app activateWithOptions:0];
        }
    };
    if ([NSThread isMainThread]) work();
    else dispatch_sync(dispatch_get_main_queue(), work);
    if (!activated)
        return {MaiErrorCode::Busy, "macOS did not activate the selected application"};
    AXUIElementRef selected = accessibilityWindow(window);
    if (!selected)
        return {MaiErrorCode::NotFound, "selected window is no longer available"};
    AXUIElementSetAttributeValue(selected, kAXMainAttribute, kCFBooleanTrue);
    AXUIElementSetAttributeValue(selected, kAXFocusedAttribute, kCFBooleanTrue);
    CFRelease(selected);
    bool foreground = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        @autoreleasepool {
            NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
            foreground = front && front.processIdentifier == pid_t(window.processId);
        }
        if (foreground) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!foreground)
        return {MaiErrorCode::Busy, "selected application did not become foreground"};
    AXUIElementRef application = AXUIElementCreateApplication(pid_t(window.processId));
    CFTypeRef focused = nullptr;
    const AXError focus = AXUIElementCopyAttributeValue(application,
        kAXFocusedWindowAttribute, &focused);
    CFRelease(application);
    if (focus != kAXErrorSuccess || !focused ||
        attribute(static_cast<AXUIElementRef>(focused), kAXTitleAttribute) != window.title) {
        if (focused) CFRelease(focused);
        return {MaiErrorCode::Busy, "selected window did not become focused"};
    }
    CFRelease(focused);
    return {};
}

MaiError maiDesktopClick(double x, double y) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    const CGPoint point = CGPointMake(x, y);
    CGEventRef down = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown,
                                              point, kCGMouseButtonLeft);
    CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp,
                                            point, kCGMouseButtonLeft);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return {MaiErrorCode::Internal, "could not create mouse event"};
    }
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    return {};
}

MaiError maiDesktopType(const std::string& text) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    const QString decoded = QString::fromUtf8(text.c_str(), int(text.size()));
    const auto* units = reinterpret_cast<const UniChar*>(decoded.utf16());
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return {MaiErrorCode::Internal, "could not create text event"};
    }
    CGEventKeyboardSetUnicodeString(down, UniCharCount(decoded.size()), units);
    CGEventKeyboardSetUnicodeString(up, UniCharCount(decoded.size()), units);
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    return {};
}

MaiError maiDesktopKey(const std::string& key) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    const std::pair<const char*, CGKeyCode> known[] = {
        {"Enter", kVK_Return}, {"Tab", kVK_Tab}, {"Escape", kVK_Escape},
        {"Backspace", kVK_Delete}, {"ArrowLeft", kVK_LeftArrow},
        {"ArrowRight", kVK_RightArrow}, {"ArrowUp", kVK_UpArrow},
        {"ArrowDown", kVK_DownArrow}};
    for (const auto& item : known) if (key == item.first) return postKey(item.second);
    return {MaiErrorCode::InvalidInput, "unsupported key"};
}

MaiError maiDesktopScroll(double x, double y, int lines) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine,
                                                     1, lines);
    if (!event) return {MaiErrorCode::Internal, "could not create scroll event"};
    CGEventSetLocation(event, CGPointMake(x, y));
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return {};
}

MaiResult<bool> maiDesktopFocusedInputIsSecure() {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    AXUIElementRef system = AXUIElementCreateSystemWide();
    if (!system) return {MaiErrorCode::Internal, "could not inspect focused input"};
    CFTypeRef focused = nullptr;
    const AXError result = AXUIElementCopyAttributeValue(system,
        kAXFocusedUIElementAttribute, &focused);
    CFRelease(system);
    if (result != kAXErrorSuccess || !focused)
        return {MaiErrorCode::NotConfigured, "focused input is not available to Accessibility"};
    const std::string role = attribute(static_cast<AXUIElementRef>(focused), kAXRoleAttribute);
    const std::string subrole = attribute(static_cast<AXUIElementRef>(focused),
                                           kAXSubroleAttribute);
    CFRelease(focused);
    return role == "AXSecureTextField" || subrole == "AXSecureTextField" ||
           subrole == "AXPasswordField";
}
