#include "remote/SecureDesktopMonitor.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace RemoteDesktop {
namespace {

#ifdef Q_OS_WIN
class WindowsSecureDesktopProbe final : public ISecureDesktopProbe {
public:
    bool isSecureDesktopActive() override {
        // 普通权限进程打不开安全桌面（Winlogon 桌面），会拿到拒绝访问。
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (desktop != nullptr) {
            CloseDesktop(desktop);
            return false;
        }
        // 只有"拒绝访问"才说明是安全桌面。其它错误（比如资源不足）不能
        // 当成安全桌面，否则会误报一堆"对方正在授权"。
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
};
#endif

// 非 Windows：没有安全桌面这回事，恒 false，让上层逻辑保持单一路径。
class NullSecureDesktopProbe final : public ISecureDesktopProbe {
public:
    bool isSecureDesktopActive() override { return false; }
};

}  // namespace

std::unique_ptr<ISecureDesktopProbe> createSecureDesktopProbe() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsSecureDesktopProbe>();
#else
    return std::make_unique<NullSecureDesktopProbe>();
#endif
}

SecureDesktopMonitor::SecureDesktopMonitor(std::unique_ptr<ISecureDesktopProbe> probe)
    : probe_(std::move(probe)) {}

SecureDesktopMonitor::Change SecureDesktopMonitor::poll(qint64 nowMs) {
    if (probe_ == nullptr) return Change::None;

    const bool observed = probe_->isSecureDesktopActive();
    if (observed == confirmedActive_) {
        // 和已确认的状态一致，说明刚才那次抖动过去了，撤销待定。
        hasPending_ = false;
        return Change::None;
    }

    if (!hasPending_ || pendingState_ != observed) {
        hasPending_ = true;
        pendingState_ = observed;
        pendingSinceMs_ = nowMs;
        return Change::None;
    }

    if (nowMs - pendingSinceMs_ < kDebounceMs) return Change::None;

    confirmedActive_ = observed;
    hasPending_ = false;
    return confirmedActive_ ? Change::Entered : Change::Left;
}

}  // namespace RemoteDesktop
