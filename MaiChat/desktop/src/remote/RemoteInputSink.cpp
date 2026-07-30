#include "remote/RemoteInputInjector.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
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
#else
    return std::make_unique<NullInputSink>();
#endif
}

bool isInputInjectionSupported() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

}  // namespace RemoteInput
