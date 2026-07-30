#include "remote/RemoteInputInjector.h"

#include <QtGlobal>

#include "remote/RemoteKeyMapping.h"

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace RemoteInput {
namespace {

// 什么都不做的兜底：非 Windows 平台，或 SendInput 不可用时。
class NullInputSink final : public IRemoteInputSink {
public:
    void moveTo(double, double) override {}
    void mouseButton(MouseButton, bool, double, double) override {}
    void wheel(int, double, double) override {}
    void key(quint32, bool) override {}
    void text(const QString&) override {}
};

#ifdef Q_OS_WIN

// 需要带 KEYEVENTF_EXTENDEDKEY 的键。不带的话方向键会被当成小键盘数字，
// 是这套 API 最经典的坑。
bool isExtendedKey(quint32 keyCode) {
    switch (keyCode) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:   // PageUp
        case VK_NEXT:    // PageDown
        case VK_INSERT:
        case VK_DELETE:
        case VK_RCONTROL:
        case VK_RMENU:   // 右 Alt
        case VK_NUMLOCK:
        case VK_SNAPSHOT:  // PrintScreen
        case VK_DIVIDE:    // 小键盘 /
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
            return true;
        default:
            return false;
    }
}

VirtualDesktopRect virtualDesktopRect() {
    VirtualDesktopRect rect;
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect;
}

// 一期只采集主屏。主屏在虚拟桌面坐标系里恒以 (0,0) 为原点。
VirtualDesktopRect primaryScreenRect() {
    VirtualDesktopRect rect;
    rect.left = 0;
    rect.top = 0;
    rect.width = GetSystemMetrics(SM_CXSCREEN);
    rect.height = GetSystemMetrics(SM_CYSCREEN);
    return rect;
}

class WindowsInputSink final : public IRemoteInputSink {
public:
    void moveTo(double x, double y) override { sendMove(x, y); }

    void mouseButton(MouseButton button, bool pressed, double x, double y) override {
        // 先移到目标位置再按：否则会在光标的旧位置上点下去。
        sendMove(x, y);

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = buttonFlag(button, pressed);
        SendInput(1, &input, sizeof(INPUT));
    }

    void wheel(int delta, double x, double y) override {
        sendMove(x, y);

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(delta);
        SendInput(1, &input, sizeof(INPUT));
    }

    void key(quint32 keyCode, bool pressed) override {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        // 用扫描码而非虚拟键码：游戏、远程终端这类程序很多只读扫描码。
        input.ki.wVk = 0;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(keyCode, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (isExtendedKey(keyCode)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (!pressed) input.ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
    }

    void text(const QString& value) override {
        if (value.isEmpty()) return;
        // KEYEVENTF_UNICODE 直接送码点，绕开被控端的键盘布局与输入法。
        // QString 本身就是 UTF-16，代理对天然被拆成两个码元依次送出。
        const QVector<quint16> units = utf16Units(value);

        QVector<INPUT> inputs;
        inputs.reserve(units.size() * 2);
        for (const quint16 unit : units) {
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wVk = 0;
            down.ki.wScan = unit;
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            inputs.append(down);

            INPUT up = down;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            inputs.append(up);
        }
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }

private:
    static QVector<quint16> utf16Units(const QString& value) {
        QVector<quint16> units;
        units.reserve(value.size());
        for (const QChar ch : value) units.append(ch.unicode());
        return units;
    }

    static DWORD buttonFlag(MouseButton button, bool pressed) {
        switch (button) {
            case MouseButton::Right:
                return pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            case MouseButton::Middle:
                return pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            case MouseButton::Left:
            default:
                return pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        }
    }

    void sendMove(double x, double y) {
        int absoluteX = 0;
        int absoluteY = 0;
        normalizedToVirtualDesktop(x, y, primaryScreenRect(), virtualDesktopRect(), &absoluteX,
                                   &absoluteY);

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx = absoluteX;
        input.mi.dy = absoluteY;
        SendInput(1, &input, sizeof(INPUT));
    }
};

#endif  // Q_OS_WIN

#ifdef Q_OS_MAC

CGPoint pointOnPrimaryScreen(double x, double y) {
    const CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    const double width = qMax<CGFloat>(1.0, CGRectGetWidth(bounds) - 1.0);
    const double height = qMax<CGFloat>(1.0, CGRectGetHeight(bounds) - 1.0);
    return CGPointMake(CGRectGetMinX(bounds) + clampNormalized(x) * width,
                       CGRectGetMinY(bounds) + clampNormalized(y) * height);
}

CGMouseButton cgMouseButton(MouseButton button) {
    switch (button) {
        case MouseButton::Right: return kCGMouseButtonRight;
        case MouseButton::Middle: return kCGMouseButtonCenter;
        case MouseButton::Left:
        default: return kCGMouseButtonLeft;
    }
}

CGEventType mouseButtonEventType(MouseButton button, bool pressed) {
    switch (button) {
        case MouseButton::Right:
            return pressed ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        case MouseButton::Middle:
            return pressed ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        case MouseButton::Left:
        default:
            return pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    }
}

class MacInputSink final : public IRemoteInputSink {
public:
    void moveTo(double x, double y) override {
        if (!AXIsProcessTrusted()) return;

        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (heldButtons_.contains(static_cast<int>(MouseButton::Left))) {
            type = kCGEventLeftMouseDragged;
        } else if (heldButtons_.contains(static_cast<int>(MouseButton::Right))) {
            type = kCGEventRightMouseDragged;
            button = kCGMouseButtonRight;
        } else if (heldButtons_.contains(static_cast<int>(MouseButton::Middle))) {
            type = kCGEventOtherMouseDragged;
            button = kCGMouseButtonCenter;
        }
        postMouseEvent(type, pointOnPrimaryScreen(x, y), button);
    }

    void mouseButton(MouseButton button, bool pressed, double x, double y) override {
        // 权限若在会话中途被撤销，抬起事件仍要清掉本地状态；否则重新授权后
        // 下一次移动会被误判成拖拽。
        if (!pressed) heldButtons_.remove(static_cast<int>(button));
        if (!AXIsProcessTrusted()) return;

        const CGPoint point = pointOnPrimaryScreen(x, y);
        postMouseEvent(mouseButtonEventType(button, pressed), point, cgMouseButton(button));
        if (pressed) {
            heldButtons_.insert(static_cast<int>(button));
        }
    }

    void wheel(int delta, double x, double y) override {
        if (!AXIsProcessTrusted() || delta == 0) return;

        int lines = delta / 120;
        if (lines == 0) lines = delta > 0 ? 1 : -1;
        CGEventRef event =
            CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, lines);
        if (!event) return;
        CGEventSetLocation(event, pointOnPrimaryScreen(x, y));
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    void key(quint32 keyCode, bool pressed) override {
        if (!AXIsProcessTrusted()) {
            // 同鼠标：权限撤销期间收到抬起，也要清掉先前记录的修饰键状态。
            if (!pressed) updateModifierFlags(keyCode, false);
            return;
        }

        const int nativeKey = macKeyCodeFromCanonical(keyCode);
        if (nativeKey < 0) return;
        updateModifierFlags(keyCode, pressed);

        CGEventRef event =
            CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(nativeKey), pressed);
        if (!event) return;
        CGEventSetFlags(event, modifierFlags_);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    void text(const QString& value) override {
        if (!AXIsProcessTrusted() || value.isEmpty()) return;

        const auto* units = reinterpret_cast<const UniChar*>(value.utf16());
        const UniCharCount length = static_cast<UniCharCount>(value.size());
        CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
        if (down) {
            CGEventKeyboardSetUnicodeString(down, length, units);
            CGEventPost(kCGHIDEventTap, down);
            CFRelease(down);
        }
        if (up) {
            CGEventKeyboardSetUnicodeString(up, length, units);
            CGEventPost(kCGHIDEventTap, up);
            CFRelease(up);
        }
    }

private:
    void postMouseEvent(CGEventType type, CGPoint point, CGMouseButton button) {
        CGEventRef event = CGEventCreateMouseEvent(nullptr, type, point, button);
        if (!event) return;
        CGEventSetFlags(event, modifierFlags_);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    void updateModifierFlags(quint32 keyCode, bool pressed) {
        CGEventFlags flag = 0;
        switch (keyCode) {
            case 0x10:
            case 0xa0:
            case 0xa1: flag = kCGEventFlagMaskShift; break;
            case 0x11:
            case 0xa2:
            case 0xa3: flag = kCGEventFlagMaskControl; break;
            case 0x12:
            case 0xa4:
            case 0xa5: flag = kCGEventFlagMaskAlternate; break;
            case 0x5b:
            case 0x5c: flag = kCGEventFlagMaskCommand; break;
            case 0x14: flag = kCGEventFlagMaskAlphaShift; break;
            default: break;
        }
        if (flag == 0) return;
        if (pressed) {
            modifierFlags_ |= flag;
        } else {
            modifierFlags_ &= ~flag;
        }
    }

    QSet<int> heldButtons_;
    CGEventFlags modifierFlags_ = 0;
};

#endif  // Q_OS_MAC

}  // namespace

void normalizedToVirtualDesktop(double normalizedX, double normalizedY,
                                const VirtualDesktopRect& capturedScreen,
                                const VirtualDesktopRect& virtualDesktop, int* outX, int* outY) {
    if (outX == nullptr || outY == nullptr) return;
    if (virtualDesktop.width <= 0 || virtualDesktop.height <= 0) {
        *outX = 0;
        *outY = 0;
        return;
    }

    // 归一化坐标 → 被采集屏幕内的像素 → 虚拟桌面内的像素。
    const double pixelX = capturedScreen.left + clampNormalized(normalizedX) * capturedScreen.width;
    const double pixelY = capturedScreen.top + clampNormalized(normalizedY) * capturedScreen.height;

    // 再归一化到 0..65535。除以 width-1 而不是 width：65535 要能落到最后
    // 一个像素上，否则永远点不到最右/最下那一列。
    const double spanX = virtualDesktop.width > 1 ? virtualDesktop.width - 1 : 1;
    const double spanY = virtualDesktop.height > 1 ? virtualDesktop.height - 1 : 1;
    const double ratioX = (pixelX - virtualDesktop.left) / spanX;
    const double ratioY = (pixelY - virtualDesktop.top) / spanY;

    *outX = qBound(0, static_cast<int>(qRound(clampNormalized(ratioX) * 65535.0)), 65535);
    *outY = qBound(0, static_cast<int>(qRound(clampNormalized(ratioY) * 65535.0)), 65535);
}

std::unique_ptr<IRemoteInputSink> createInputSink() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsInputSink>();
#elif defined(Q_OS_MAC)
    return std::make_unique<MacInputSink>();
#else
    return std::make_unique<NullInputSink>();
#endif
}

bool isInputInjectionSupported() {
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    return true;
#else
    return false;
#endif
}

bool hasInputInjectionPermission() {
#ifdef Q_OS_MAC
    return AXIsProcessTrusted();
#else
    return isInputInjectionSupported();
#endif
}

void requestInputInjectionPermission() {
#ifdef Q_OS_MAC
    const void* keys[] = {kAXTrustedCheckOptionPrompt};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef options =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFCopyStringDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    if (options) {
        AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
    }
#endif
}

QString describeInjectionGeometry() {
#ifdef Q_OS_WIN
    const VirtualDesktopRect primary = primaryScreenRect();
    const VirtualDesktopRect virtualDesktop = virtualDesktopRect();

    // GetSystemMetrics 返回的是**逻辑**分辨率：进程若不是 DPI 感知的，
    // 2560x1600 会被报成 1707x1067。EnumDisplaySettings 拿的是真实物理模式，
    // 两个都打出来，一眼就能看出当前进程处在哪种状态——这个歧义在排查里
    // 已经误导过一次，不能只留一个数。
    QString physical = QStringLiteral("unknown");
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        physical = QStringLiteral("%1x%2").arg(mode.dmPelsWidth).arg(mode.dmPelsHeight);
    }

    return QStringLiteral("primary=%1x%2 physical=%3 virtualDesktop=(%4,%5 %6x%7) "
                          "aspect=%8 monitors=%9 dpiAware=%10")
        .arg(primary.width)
        .arg(primary.height)
        .arg(physical)
        .arg(virtualDesktop.left)
        .arg(virtualDesktop.top)
        .arg(virtualDesktop.width)
        .arg(virtualDesktop.height)
        .arg(primary.height > 0 ? QString::number(
                                      static_cast<double>(primary.width) / primary.height, 'f', 4)
                                : QStringLiteral("n/a"))
        .arg(GetSystemMetrics(SM_CMONITORS))
        .arg(physical == QStringLiteral("%1x%2").arg(primary.width).arg(primary.height)
                 ? QStringLiteral("true")
                 : QStringLiteral("false"));
#elif defined(Q_OS_MAC)
    const CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    return QStringLiteral("primary=%1x%2 aspect=%3 accessibility=%4")
        .arg(static_cast<int>(CGRectGetWidth(bounds)))
        .arg(static_cast<int>(CGRectGetHeight(bounds)))
        .arg(CGRectGetHeight(bounds) > 0
                 ? QString::number(CGRectGetWidth(bounds) / CGRectGetHeight(bounds), 'f', 4)
                 : QStringLiteral("n/a"))
        .arg(hasInputInjectionPermission() ? QStringLiteral("granted") : QStringLiteral("DENIED"));
#else
    return QStringLiteral("input injection not supported on this platform");
#endif
}

}  // namespace RemoteInput
