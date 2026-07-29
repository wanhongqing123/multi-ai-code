#pragma once

#include <memory>

#include <QtGlobal>

// 安全桌面探测：UAC 提权框、锁屏、Ctrl+Alt+Del 界面出现时，Windows 会切到
// 一个独立的「安全桌面」。那上面我们**既采不到画面、也注不进输入**——
// 远程会表现为"画面卡住 + 点不动"。
//
// 一期不打算做系统服务去进那个桌面（那是另一个量级的工程），但至少要让
// 控制端知道发生了什么，而不是对着一块卡住的画面猜是断网还是崩了。
//
// 分两层：探测（平台相关，薄）与判定（纯逻辑，可测）。
namespace RemoteDesktop {

class ISecureDesktopProbe {
public:
    virtual ~ISecureDesktopProbe() = default;
    // 当前的「输入桌面」是不是安全桌面。
    virtual bool isSecureDesktopActive() = 0;
};

// Windows 上返回真实探测；其它平台返回恒 false 的实现（不影响编译与逻辑）。
std::unique_ptr<ISecureDesktopProbe> createSecureDesktopProbe();

class SecureDesktopMonitor {
public:
    enum class Change {
        None,     // 状态没变，或还在防抖窗口里没坐实
        Entered,  // 刚进入安全桌面：该提示控制端了
        Left      // 刚离开：该收起提示了
    };

    explicit SecureDesktopMonitor(std::unique_ptr<ISecureDesktopProbe> probe);

    // 由定时器驱动。只在状态**稳定翻转**时返回非 None，不会每次轮询都报。
    Change poll(qint64 nowMs);

    bool isActive() const { return confirmedActive_; }

    // 桌面切换有个短暂过渡期，探测也可能瞬时抖动。要求新状态连续稳定这么久
    // 才认账，避免提示条闪来闪去。
    static constexpr qint64 kDebounceMs = 600;

private:
    std::unique_ptr<ISecureDesktopProbe> probe_;
    bool confirmedActive_ = false;
    bool pendingState_ = false;
    bool hasPending_ = false;
    qint64 pendingSinceMs_ = 0;
};

}  // namespace RemoteDesktop
