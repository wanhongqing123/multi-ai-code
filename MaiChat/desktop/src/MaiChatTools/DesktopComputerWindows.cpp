#include "MaiChatTools/DesktopComputerPlatform.h"

#include <windows.h>
#include <dwmapi.h>
#include <UIAutomation.h>
#include <UIAutomationClient.h>
#include <oleauto.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <QString>

namespace {

std::string utf8(const std::wstring& value) {
    return QString::fromStdWString(value).toUtf8().toStdString();
}

std::wstring wide(const std::string& value) {
    return QString::fromUtf8(value.c_str(), int(value.size())).toStdWString();
}

HWND handle(const DesktopNativeWindow& window) {
    return reinterpret_cast<HWND>(std::uintptr_t(window.id));
}

RECT visibleBounds(HWND window) {
    RECT bounds{};
    if (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                              &bounds, sizeof(bounds)) != S_OK)
        GetWindowRect(window, &bounds);
    return bounds;
}

MaiError sendInputs(std::vector<INPUT>& inputs) {
    if (inputs.empty() || SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size())
        return {MaiErrorCode::Internal, "Windows did not accept the input events"};
    return {};
}

struct ComScope {
    bool uninitialize = false;
    ComScope() {
        const HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize = SUCCEEDED(status);
    }
    ~ComScope() { if (uninitialize) CoUninitialize(); }
};

MaiResult<IUIAutomation*> automation() {
    IUIAutomation* value = nullptr;
    const HRESULT status = CoCreateInstance(CLSID_CUIAutomation, nullptr,
        CLSCTX_INPROC_SERVER, IID_IUIAutomation, reinterpret_cast<void**>(&value));
    if (FAILED(status) || !value)
        return {MaiErrorCode::NotConfigured, "Windows UI Automation is unavailable"};
    return value;
}

std::string controlRole(CONTROLTYPEID type) {
    switch (type) {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_EditControlTypeId: return "edit";
    case UIA_TextControlTypeId: return "text";
    case UIA_HyperlinkControlTypeId: return "link";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_ComboBoxControlTypeId: return "combobox";
    case UIA_ListItemControlTypeId: return "listitem";
    case UIA_MenuItemControlTypeId: return "menuitem";
    case UIA_TabItemControlTypeId: return "tabitem";
    case UIA_WindowControlTypeId: return "window";
    default: return "control:" + std::to_string(type);
    }
}

} // namespace

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows() {
    std::vector<DesktopNativeWindow> windows;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* output = reinterpret_cast<std::vector<DesktopNativeWindow>*>(parameter);
        if (output->size() >= 200) return FALSE;
        if (!IsWindowVisible(window) || IsIconic(window) ||
            GetWindowTextLengthW(window) <= 0) return TRUE;
        const int length = GetWindowTextLengthW(window);
        std::vector<wchar_t> title(std::size_t(length + 1));
        if (GetWindowTextW(window, title.data(), length + 1) <= 0) return TRUE;
        RECT rectangle = visibleBounds(window);
        const int width = rectangle.right - rectangle.left;
        const int height = rectangle.bottom - rectangle.top;
        if (width < 80 || height < 50) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0) return TRUE;
        DesktopNativeWindow candidate;
        candidate.id = std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
        candidate.processId = pid;
        candidate.title = utf8(title.data());
        candidate.x = rectangle.left;
        candidate.y = rectangle.top;
        candidate.width = width;
        candidate.height = height;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            wchar_t path[32768]{};
            DWORD count = DWORD(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &count))
                candidate.application = utf8(std::wstring(path, count));
            CloseHandle(process);
        }
        output->push_back(std::move(candidate));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(
    const DesktopNativeWindow& window) {
    ComScope com;
    const auto opened = automation();
    if (!opened) return opened.error();
    IUIAutomation* ui = opened.value();
    IUIAutomationElement* root = nullptr;
    const HRESULT found = ui->ElementFromHandle(handle(window), &root);
    if (FAILED(found) || !root) {
        ui->Release();
        return {MaiErrorCode::NotFound, "selected window is unavailable to UI Automation"};
    }
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(ui->get_ControlViewWalker(&walker)) || !walker) {
        root->Release();
        ui->Release();
        return {MaiErrorCode::NotConfigured, "UI Automation walker is unavailable"};
    }
    std::vector<DesktopNativeElement> output;
    std::vector<IUIAutomationElement*> pending{root};
    for (std::size_t index = 0; index < pending.size() && index < 250; ++index) {
        IUIAutomationElement* current = pending[index];
        DesktopNativeElement item;
        BSTR name = nullptr;
        if (SUCCEEDED(current->get_CurrentName(&name)) && name) {
            if (SysStringLen(name) <= 4096)
                item.label = utf8(std::wstring(name, SysStringLen(name)));
            SysFreeString(name);
        }
        CONTROLTYPEID type = 0;
        current->get_CurrentControlType(&type);
        item.role = controlRole(type);
        RECT frame{};
        if (!item.label.empty() &&
            SUCCEEDED(current->get_CurrentBoundingRectangle(&frame)) &&
            frame.right > frame.left && frame.bottom > frame.top &&
            output.size() < 80) {
            item.x = frame.left;
            item.y = frame.top;
            item.width = frame.right - frame.left;
            item.height = frame.bottom - frame.top;
            output.push_back(std::move(item));
        }
        IUIAutomationElement* child = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(current, &child)) && child) {
            while (child && pending.size() < 250) {
                pending.push_back(child);
                IUIAutomationElement* next = nullptr;
                walker->GetNextSiblingElement(child, &next);
                child = next;
            }
            if (child) child->Release();
        }
        current->Release();
    }
    walker->Release();
    ui->Release();
    return output;
}

MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window) {
    HWND selected = handle(window);
    if (!IsWindow(selected) || !IsWindowVisible(selected) || IsIconic(selected))
        return {MaiErrorCode::NotFound, "selected Windows window is no longer visible"};
    SetForegroundWindow(selected);
    if (GetForegroundWindow() != selected)
        return {MaiErrorCode::Busy, "Windows did not allow the selected window to become foreground"};
    return {};
}

MaiError maiDesktopClick(double x, double y) {
    if (!SetCursorPos(int(std::lround(x)), int(std::lround(y))))
        return {MaiErrorCode::Internal, "Windows could not move the pointer"};
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    INPUT up = down;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    std::vector<INPUT> events{down, up};
    return sendInputs(events);
}

MaiError maiDesktopType(const std::string& text) {
    const QString decoded = QString::fromUtf8(text.c_str(), int(text.size()));
    std::vector<INPUT> events;
    events.reserve(std::size_t(decoded.size()) * 2);
    for (QChar unit : decoded) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = unit.unicode();
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        events.push_back(down);
        events.push_back(up);
    }
    return sendInputs(events);
}

MaiError maiDesktopKey(const std::string& key) {
    const std::pair<const char*, WORD> known[] = {
        {"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Escape", VK_ESCAPE},
        {"Backspace", VK_BACK}, {"ArrowLeft", VK_LEFT},
        {"ArrowRight", VK_RIGHT}, {"ArrowUp", VK_UP},
        {"ArrowDown", VK_DOWN}};
    for (const auto& item : known) {
        if (key != item.first) continue;
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = item.second;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        std::vector<INPUT> events{down, up};
        return sendInputs(events);
    }
    return {MaiErrorCode::InvalidInput, "unsupported key"};
}

MaiError maiDesktopScroll(double x, double y, int lines) {
    if (!SetCursorPos(int(std::lround(x)), int(std::lround(y))))
        return {MaiErrorCode::Internal, "Windows could not move the pointer"};
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = MOUSEEVENTF_WHEEL;
    event.mi.mouseData = DWORD(lines * WHEEL_DELTA);
    std::vector<INPUT> events{event};
    return sendInputs(events);
}

MaiResult<bool> maiDesktopFocusedInputIsSecure() {
    ComScope com;
    const auto opened = automation();
    if (!opened) return opened.error();
    IUIAutomation* ui = opened.value();
    IUIAutomationElement* focused = nullptr;
    const HRESULT status = ui->GetFocusedElement(&focused);
    ui->Release();
    if (FAILED(status) || !focused)
        return {MaiErrorCode::NotConfigured, "focused input is unavailable to UI Automation"};
    BOOL protectedInput = FALSE;
    const HRESULT secure = focused->get_CurrentIsPassword(&protectedInput);
    focused->Release();
    if (FAILED(secure))
        return {MaiErrorCode::NotConfigured, "could not verify whether focused input is secure"};
    return protectedInput != FALSE;
}
