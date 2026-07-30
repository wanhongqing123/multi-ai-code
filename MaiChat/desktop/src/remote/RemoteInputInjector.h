#pragma once

#include <QSet>
#include <QString>
#include <QVector>

#include <memory>

#include "remote/RemoteInputProtocol.h"

// 被控端输入注入。
//
// 分两层：
//   IRemoteInputSink   —— 真正落到系统的原子操作（Windows SendInput / macOS CGEvent）
//   RemoteInputInjector —— 会话绑定、按住键跟踪、丢包兜底、危险组合键拦截
//
// 所有判断逻辑都在 Injector 里且不依赖平台 API，可以拿 Fake sink 完整单测；
// 平台相关的部分被压缩到 Sink 那一层，薄到不需要测。
namespace RemoteInput {

// Channel 定义在 RemoteInputProtocol.h：收发两端都要按通道分开处理，
// 它是协议层的概念，不属于注入器。

class IRemoteInputSink {
public:
    virtual ~IRemoteInputSink() = default;

    // 坐标是相对被采集屏幕的归一化值 [0,1]。
    virtual void moveTo(double x, double y) = 0;
    virtual void mouseButton(MouseButton button, bool pressed, double x, double y) = 0;
    virtual void wheel(int delta, double x, double y) = 0;
    virtual void key(quint32 keyCode, bool pressed) = 0;
    virtual void text(const QString& text) = 0;
};

class RemoteInputInjector {
public:
    explicit RemoteInputInjector(std::unique_ptr<IRemoteInputSink> sink);

    // 会话结束一律把按住的东西全抬起来。控制端崩了、网断了、超时了，
    // 被控端都不该留着 Ctrl 按住的状态——人不在电脑旁，没法自己解。
    void beginSession(const QString& sessionId);
    void endSession();
    bool isSessionActive() const { return sessionActive_; }

    // 返回 false 表示整包被拒（无会话 / 会话 ID 不符 / 过期包）。
    bool handlePacket(const Packet& packet, Channel channel, qint64 nowMs);

    // 由定时器驱动。静默超时且仍有键按住时全部抬起。
    void tickWatchdog(qint64 nowMs);

    QVector<quint32> heldKeys() const;
    QVector<MouseButton> heldButtons() const;
    bool hasAnythingHeld() const;

    // 静默多久后认定链路已断，把按住的键全抬了。
    static constexpr qint64 kSilenceTimeoutMs = 5000;
    // 可靠通道允许的最大跳号。超过就先全抬——中间那些抬起包大概率已经丢了。
    static constexpr quint32 kMaxSequenceGap = 1;

private:
    void applyEvent(const Event& event);
    void releaseAllHeld();

    std::unique_ptr<IRemoteInputSink> sink_;
    QString sessionId_;
    bool sessionActive_ = false;
    QSet<quint32> heldKeys_;
    QSet<int> heldButtons_;
    bool hasUnreliableSequence_ = false;
    quint32 lastUnreliableSequence_ = 0;
    bool hasReliableSequence_ = false;
    quint32 lastReliableSequence_ = 0;
    qint64 lastPacketMs_ = 0;
};

// Win+L 会让被控机立刻进入安全桌面，SendInput 够不着，远程直接失联且再也解不开。
// 它是注得进去的，所以必须主动拦。
bool isBlockedKeyCombination(quint32 keyCode, const QSet<quint32>& heldKeys);

// 被采集屏幕内的归一化坐标 → SendInput 的虚拟桌面绝对坐标（0..65535）。
//
// SendInput 带 MOUSEEVENTF_VIRTUALDESK 时收的就是这个 0..65535 的归一化值，
// **不是像素**，所以多显示器与 DPI 缩放都由 Windows 自己处理。但归一化的
// 基准是**整个虚拟桌面**，而我们只采集主屏，所以要先换算到虚拟桌面坐标系。
//
// 纯函数，单独抽出来是因为这段最容易算错，且不需要真的动鼠标才能测。
struct VirtualDesktopRect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};
void normalizedToVirtualDesktop(double normalizedX, double normalizedY,
                                const VirtualDesktopRect& capturedScreen,
                                const VirtualDesktopRect& virtualDesktop, int* outX, int* outY);

// Windows 返回 SendInput 实现，macOS 返回 CGEvent 实现。
std::unique_ptr<IRemoteInputSink> createInputSink();
// 当前平台是否实现了系统级输入注入。观看端发送输入不受此限制。
bool isInputInjectionSupported();
// macOS 注入需要“辅助功能”权限；Windows 始终返回 true。
bool hasInputInjectionPermission();
// macOS 弹出系统授权引导；其它平台为空操作。
void requestInputInjectionPermission();

}  // namespace RemoteInput
