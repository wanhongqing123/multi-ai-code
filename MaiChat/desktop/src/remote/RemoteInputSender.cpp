#include "remote/RemoteInputSender.h"

namespace RemoteInput {

QRectF fitContentRect(const QSizeF& widgetSize, const QSizeF& videoSize) {
    if (widgetSize.width() <= 0.0 || widgetSize.height() <= 0.0 || videoSize.width() <= 0.0
        || videoSize.height() <= 0.0) {
        return QRectF();
    }

    const double widgetAspect = widgetSize.width() / widgetSize.height();
    const double videoAspect = videoSize.width() / videoSize.height();
    double width = widgetSize.width();
    double height = widgetSize.height();
    if (videoAspect > widgetAspect) {
        // 画面比控件更宽：宽度顶满，上下留黑边。
        height = widgetSize.width() / videoAspect;
    } else {
        // 画面比控件更高（或等比）：高度顶满，左右留黑边。
        width = widgetSize.height() * videoAspect;
    }
    return QRectF((widgetSize.width() - width) / 2.0, (widgetSize.height() - height) / 2.0,
                  width, height);
}

void RemoteInputSender::beginSession(const QString& sessionId) {
    sessionId_ = sessionId;
    sessionActive_ = true;
    unreliableSequence_ = 0;
    reliableSequence_ = 0;
    tokens_ = static_cast<double>(kBudgetPerSecond);
    lastRefillMs_ = 0;
    lastMoveSentMs_ = 0;
    resetQueues();
}

void RemoteInputSender::endSession() {
    sessionActive_ = false;
    sessionId_.clear();
    resetQueues();
}

void RemoteInputSender::resetQueues() {
    hasPendingMove_ = false;
    reliableQueue_.clear();
}

void RemoteInputSender::setContentRect(const QRectF& contentRect) {
    contentRect_ = contentRect;
}

bool RemoteInputSender::mapToNormalized(const QPointF& widgetPos, double* outX,
                                        double* outY) const {
    if (outX == nullptr || outY == nullptr) return false;
    if (contentRect_.width() <= 0.0 || contentRect_.height() <= 0.0) return false;
    if (!contentRect_.contains(widgetPos)) return false;
    *outX = clampNormalized((widgetPos.x() - contentRect_.x()) / contentRect_.width());
    *outY = clampNormalized((widgetPos.y() - contentRect_.y()) / contentRect_.height());
    return true;
}

bool RemoteInputSender::mapToNormalizedClamped(const QPointF& widgetPos, double* outX,
                                               double* outY) const {
    if (outX == nullptr || outY == nullptr) return false;
    if (contentRect_.width() <= 0.0 || contentRect_.height() <= 0.0) return false;
    *outX = clampNormalized((widgetPos.x() - contentRect_.x()) / contentRect_.width());
    *outY = clampNormalized((widgetPos.y() - contentRect_.y()) / contentRect_.height());
    return true;
}

void RemoteInputSender::queueMouseMove(double x, double y) {
    if (!sessionActive_) return;
    // 只留最新位置：中间点发过去也会被立刻覆盖，白占配额。
    hasPendingMove_ = true;
    pendingMoveX_ = clampNormalized(x);
    pendingMoveY_ = clampNormalized(y);
}

void RemoteInputSender::queueMouseButton(MouseButton button, bool pressed, double x, double y) {
    if (!sessionActive_) return;
    Event event;
    event.type = EventType::MouseButton;
    event.button = button;
    event.pressed = pressed;
    event.x = clampNormalized(x);
    event.y = clampNormalized(y);
    reliableQueue_.append(event);
    // 按键自带坐标，等于顺带把位置也同步了，待发的移动就没必要再占一个包。
    hasPendingMove_ = false;
}

void RemoteInputSender::queueWheel(int delta, double x, double y) {
    if (!sessionActive_) return;
    // 同一 tick 内的滚轮按增量求和：一次快速滚动几十个事件也只占一个包。
    for (auto& event : reliableQueue_) {
        if (event.type == EventType::MouseWheel) {
            event.wheelDelta += delta;
            event.x = clampNormalized(x);
            event.y = clampNormalized(y);
            return;
        }
    }
    Event event;
    event.type = EventType::MouseWheel;
    event.wheelDelta = delta;
    event.x = clampNormalized(x);
    event.y = clampNormalized(y);
    reliableQueue_.append(event);
}

void RemoteInputSender::queueKey(quint32 keyCode, bool pressed) {
    if (!sessionActive_) return;
    Event event;
    event.type = EventType::Key;
    event.keyCode = keyCode;
    event.pressed = pressed;
    reliableQueue_.append(event);
}

void RemoteInputSender::queueText(const QString& text) {
    if (!sessionActive_ || text.isEmpty()) return;
    Event event;
    event.type = EventType::Text;
    event.text = text;
    reliableQueue_.append(event);
}

void RemoteInputSender::queueReleaseAll() {
    if (!sessionActive_) return;
    Event event;
    event.type = EventType::ReleaseAll;
    reliableQueue_.append(event);
}

QVector<RemoteInputSender::OutgoingPacket> RemoteInputSender::flush(qint64 nowMs) {
    QVector<OutgoingPacket> outgoing;
    if (!sessionActive_) return outgoing;

    if (lastRefillMs_ == 0) lastRefillMs_ = nowMs;
    const qint64 elapsed = nowMs - lastRefillMs_;
    if (elapsed > 0) {
        tokens_ = qMin<double>(static_cast<double>(kBudgetPerSecond),
                               tokens_ + elapsed * kBudgetPerSecond / 1000.0);
        lastRefillMs_ = nowMs;
    }

    // 按键优先：预算紧张时该牺牲的是移动（下一包就纠正回来），不是按键
    // （丢了会留下悬空状态）。所以先发可靠队列。
    while (!reliableQueue_.isEmpty() && tokens_ >= 1.0) {
        Packet packet;
        packet.sessionId = sessionId_;
        packet.sequence = reliableSequence_ + 1;

        // 尽量多塞几条：1KB 装得下几十条事件，合批能把配额省下来给移动。
        int taken = 0;
        for (const auto& event : reliableQueue_) {
            Packet candidate = packet;
            candidate.events.append(event);
            if (!fitsInOnePacket(candidate)) break;
            packet.events.append(event);
            ++taken;
        }
        if (taken == 0) {
            // 单条事件就撑破 1KB（超长文本）。它永远塞不进去，丢掉并继续，
            // 否则整条队列会被它堵死。
            reliableQueue_.removeFirst();
            continue;
        }

        reliableQueue_.remove(0, taken);
        reliableSequence_ = packet.sequence;
        tokens_ -= 1.0;
        outgoing.append({Channel::Reliable, encodePacket(packet)});
    }

    // 移动用剩余令牌，且必须给按键留出爆发余量。
    const bool moveIntervalElapsed =
        lastMoveSentMs_ == 0 || nowMs - lastMoveSentMs_ >= kMoveIntervalMs;
    if (hasPendingMove_ && moveIntervalElapsed && tokens_ >= kMoveMinTokens) {
        Event move;
        move.type = EventType::MouseMove;
        move.x = pendingMoveX_;
        move.y = pendingMoveY_;

        Packet packet;
        packet.sessionId = sessionId_;
        packet.sequence = ++unreliableSequence_;
        packet.events = {move};

        hasPendingMove_ = false;
        lastMoveSentMs_ = nowMs;
        tokens_ -= 1.0;
        outgoing.append({Channel::Unreliable, encodePacket(packet)});
    }

    return outgoing;
}

}  // namespace RemoteInput
