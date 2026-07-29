#include "remote/RemoteInputInjector.h"

namespace RemoteInput {
namespace {

// Windows 虚拟键码。这里直接写字面量而不 include <windows.h>：
// 本文件要在无 Windows 头的环境下参与单测，且这几个值是稳定的公开常量。
constexpr quint32 kVkLeftWin = 0x5B;
constexpr quint32 kVkRightWin = 0x5C;
constexpr quint32 kVkL = 0x4C;

int buttonIndex(MouseButton button) {
    switch (button) {
        case MouseButton::Left: return 0;
        case MouseButton::Right: return 1;
        case MouseButton::Middle: return 2;
    }
    return 0;
}

MouseButton buttonFromIndex(int index) {
    switch (index) {
        case 1: return MouseButton::Right;
        case 2: return MouseButton::Middle;
        default: return MouseButton::Left;
    }
}

}  // namespace

bool isBlockedKeyCombination(quint32 keyCode, const QSet<quint32>& heldKeys) {
    if (keyCode != kVkL) return false;
    return heldKeys.contains(kVkLeftWin) || heldKeys.contains(kVkRightWin);
}

RemoteInputInjector::RemoteInputInjector(std::unique_ptr<IRemoteInputSink> sink)
    : sink_(std::move(sink)) {}

void RemoteInputInjector::beginSession(const QString& sessionId) {
    // 新会话开始前先清干净：上一场若是异常结束，可能还留着按住的键。
    releaseAllHeld();
    sessionId_ = sessionId;
    sessionActive_ = true;
    hasUnreliableSequence_ = false;
    lastUnreliableSequence_ = 0;
    hasReliableSequence_ = false;
    lastReliableSequence_ = 0;
    lastPacketMs_ = 0;
}

void RemoteInputInjector::endSession() {
    releaseAllHeld();
    sessionActive_ = false;
    sessionId_.clear();
}

bool RemoteInputInjector::handlePacket(const Packet& packet, Channel channel, qint64 nowMs) {
    if (!sessionActive_ || sink_ == nullptr) return false;
    // 会话 ID 不符一律拒收：上一场会话的残留包不该操作这一场的电脑。
    if (packet.sessionId != sessionId_) return false;

    if (channel == Channel::Unreliable) {
        // 不可靠通道乱序到达是常态。旧位置盖掉新位置会让光标来回跳，直接丢弃。
        // 丢包本身不需要任何兜底——下一包就把位置纠正回来了。
        if (hasUnreliableSequence_ && packet.sequence <= lastUnreliableSequence_) return false;
        hasUnreliableSequence_ = true;
        lastUnreliableSequence_ = packet.sequence;
    } else {
        // 可靠有序通道本不该跳号。跳了说明链路真出了问题，中间那些抬起包
        // 多半已经丢了，先把按住的全抬掉再继续，避免留下悬空状态。
        if (hasReliableSequence_ && packet.sequence > lastReliableSequence_ + kMaxSequenceGap) {
            releaseAllHeld();
        }
        if (hasReliableSequence_ && packet.sequence <= lastReliableSequence_) return false;
        hasReliableSequence_ = true;
        lastReliableSequence_ = packet.sequence;
    }

    lastPacketMs_ = nowMs;
    for (const auto& event : packet.events) applyEvent(event);
    return true;
}

void RemoteInputInjector::applyEvent(const Event& event) {
    switch (event.type) {
        case EventType::MouseMove:
            sink_->moveTo(event.x, event.y);
            break;
        case EventType::MouseButton:
            sink_->mouseButton(event.button, event.pressed, event.x, event.y);
            if (event.pressed) {
                heldButtons_.insert(buttonIndex(event.button));
            } else {
                heldButtons_.remove(buttonIndex(event.button));
            }
            break;
        case EventType::MouseWheel:
            sink_->wheel(event.wheelDelta, event.x, event.y);
            break;
        case EventType::Key:
            // 按下被拦时连抬起一并吞掉：只拦按下会让对端以为键还按着，
            // 而本地根本没按下去，状态就对不上了。
            if (isBlockedKeyCombination(event.keyCode, heldKeys_)) return;
            sink_->key(event.keyCode, event.pressed);
            if (event.pressed) {
                heldKeys_.insert(event.keyCode);
            } else {
                heldKeys_.remove(event.keyCode);
            }
            break;
        case EventType::Text:
            sink_->text(event.text);
            break;
        case EventType::ReleaseAll:
            releaseAllHeld();
            break;
        case EventType::Unknown:
            break;
    }
}

void RemoteInputInjector::tickWatchdog(qint64 nowMs) {
    if (!sessionActive_ || !hasAnythingHeld()) return;
    // 收到过包才谈得上静默；lastPacketMs_ 为 0 说明这一场还没收到任何输入。
    if (lastPacketMs_ == 0) return;
    if (nowMs - lastPacketMs_ < kSilenceTimeoutMs) return;
    releaseAllHeld();
}

void RemoteInputInjector::releaseAllHeld() {
    if (sink_ == nullptr) return;
    // 拷贝一份再遍历：sink 回调理论上可能反过来动到这两个集合。
    const QSet<quint32> keys = heldKeys_;
    const QSet<int> buttons = heldButtons_;
    heldKeys_.clear();
    heldButtons_.clear();
    for (const auto keyCode : keys) sink_->key(keyCode, false);
    for (const auto button : buttons) {
        // 抬起位置无所谓，按下时已经在目标位置了；这里只求把键放开。
        sink_->mouseButton(buttonFromIndex(button), false, 0.0, 0.0);
    }
}

QVector<quint32> RemoteInputInjector::heldKeys() const {
    QVector<quint32> result;
    result.reserve(heldKeys_.size());
    for (const auto keyCode : heldKeys_) result.append(keyCode);
    std::sort(result.begin(), result.end());
    return result;
}

QVector<MouseButton> RemoteInputInjector::heldButtons() const {
    QVector<int> indices;
    indices.reserve(heldButtons_.size());
    for (const auto index : heldButtons_) indices.append(index);
    std::sort(indices.begin(), indices.end());

    QVector<MouseButton> result;
    result.reserve(indices.size());
    for (const auto index : indices) result.append(buttonFromIndex(index));
    return result;
}

bool RemoteInputInjector::hasAnythingHeld() const {
    return !heldKeys_.isEmpty() || !heldButtons_.isEmpty();
}

}  // namespace RemoteInput
