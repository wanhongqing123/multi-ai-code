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

CaptureCoordinateMapping calculateCaptureCoordinateMapping(
    const QSizeF& surfaceSize, const QSizeF& encodedFrameSize,
    const std::optional<RemoteDesktop::CaptureGeometry>& captureGeometry) {
    CaptureCoordinateMapping mapping;
    mapping.surfaceVideoRect = fitContentRect(surfaceSize, encodedFrameSize);
    if (mapping.surfaceVideoRect.isEmpty()) return mapping;

    mapping.encodedContentRect =
        QRectF(QPointF(0.0, 0.0), encodedFrameSize);
    mapping.surfaceContentRect = mapping.surfaceVideoRect;
    if (!captureGeometry || !captureGeometry->isValid()
        || captureGeometry->contentMode != RemoteDesktop::CaptureContentMode::Fit) {
        return mapping;
    }

    // 第一级：采集区域在编码帧内的 active rect。若实验接口成功让编码输出跟随
    // 采集源比例，这里正好是整帧；旧 SDK 仍输出固定 16:9 时，这里会剥掉帧内黑边。
    mapping.encodedContentRect = fitContentRect(
        encodedFrameSize, QSizeF(captureGeometry->captureRect.size()));
    if (mapping.encodedContentRect.isEmpty()) return mapping;

    // 第二级：TRTC Fit 渲染把整个编码帧放进 Qt surface。按编码帧比例把 active
    // rect 投影进去，得到真正能接收输入的最终矩形。
    const double scaleX = mapping.surfaceVideoRect.width() / encodedFrameSize.width();
    const double scaleY = mapping.surfaceVideoRect.height() / encodedFrameSize.height();
    mapping.surfaceContentRect = QRectF(
        mapping.surfaceVideoRect.x() + mapping.encodedContentRect.x() * scaleX,
        mapping.surfaceVideoRect.y() + mapping.encodedContentRect.y() * scaleY,
        mapping.encodedContentRect.width() * scaleX,
        mapping.encodedContentRect.height() * scaleY);
    mapping.usesCaptureGeometry = true;
    return mapping;
}

void RemoteInputSender::beginSession(const QString& sessionId) {
    sessionId_ = sessionId;
    sessionActive_ = true;
    unreliableSequence_ = 0;
    reliableSequence_ = 0;
    sendHistory_.clear();
    lastMoveSentMs_ = 0;
    resetQueues();
}

void RemoteInputSender::endSession() {
    sessionActive_ = false;
    sessionId_.clear();
    resetQueues();
}

void RemoteInputSender::resetQueues() {
    pendingMovePath_.clear();
    reliableQueue_.clear();
}

void RemoteInputSender::setContentRect(const QRectF& contentRect) {
    contentRect_ = contentRect;
}

void RemoteInputSender::setCaptureGeometry(
    const std::optional<RemoteDesktop::CaptureGeometry>& captureGeometry) {
    captureGeometry_ = captureGeometry && captureGeometry->isValid()
        ? captureGeometry
        : std::nullopt;
}

QRectF RemoteInputSender::normalizedTargetRect() const {
    if (!captureGeometry_ || !captureGeometry_->isValid()) return QRectF(0.0, 0.0, 1.0, 1.0);
    const auto& geometry = *captureGeometry_;
    return QRectF(
        static_cast<double>(geometry.captureRect.x()) / geometry.sourceSize.width(),
        static_cast<double>(geometry.captureRect.y()) / geometry.sourceSize.height(),
        static_cast<double>(geometry.captureRect.width()) / geometry.sourceSize.width(),
        static_cast<double>(geometry.captureRect.height()) / geometry.sourceSize.height());
}

bool RemoteInputSender::mapToNormalized(const QPointF& widgetPos, double* outX,
                                        double* outY) const {
    if (outX == nullptr || outY == nullptr) return false;
    if (contentRect_.width() <= 0.0 || contentRect_.height() <= 0.0) return false;
    if (!contentRect_.contains(widgetPos)) return false;
    const double localX =
        clampNormalized((widgetPos.x() - contentRect_.x()) / contentRect_.width());
    const double localY =
        clampNormalized((widgetPos.y() - contentRect_.y()) / contentRect_.height());
    const QRectF target = normalizedTargetRect();
    *outX = clampNormalized(target.x() + localX * target.width());
    *outY = clampNormalized(target.y() + localY * target.height());
    return true;
}

bool RemoteInputSender::mapToNormalizedClamped(const QPointF& widgetPos, double* outX,
                                               double* outY) const {
    if (outX == nullptr || outY == nullptr) return false;
    if (contentRect_.width() <= 0.0 || contentRect_.height() <= 0.0) return false;
    const double localX =
        clampNormalized((widgetPos.x() - contentRect_.x()) / contentRect_.width());
    const double localY =
        clampNormalized((widgetPos.y() - contentRect_.y()) / contentRect_.height());
    const QRectF target = normalizedTargetRect();
    *outX = clampNormalized(target.x() + localX * target.width());
    *outY = clampNormalized(target.y() + localY * target.height());
    return true;
}

void RemoteInputSender::queueMouseMove(double x, double y) {
    if (!sessionActive_) return;
    // 攒的是整段轨迹而不是最后一点：拖拽画线、框选时中间点就是内容本身，
    // 只留终点会把曲线拉成直线。一个包能带十几个点，不额外占配额。
    pendingMovePath_.append(QPointF(clampNormalized(x), clampNormalized(y)));
    if (pendingMovePath_.size() > kMaxMovePointsPerPacket) {
        // 超出单包容量时抽稀中间点，但**终点必须保留**——终点才是光标最终
        // 该停的位置，丢了会让光标停在半路。
        pendingMovePath_.remove(pendingMovePath_.size() / 2);
    }
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
    // 按键自带坐标，等于顺带把位置也同步了，待发的轨迹就没必要再占一个包。
    pendingMovePath_.clear();
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

    // 按键优先：预算紧张时该牺牲的是移动（下一包就纠正回来），不是按键
    // （丢了会留下悬空状态）。所以先发可靠队列。
    while (!reliableQueue_.isEmpty()) {
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

        const QByteArray payload = encodePacket(packet);
        if (!canSend(nowMs, payload.size(), kBudgetPerSecond)) break;

        reliableQueue_.remove(0, taken);
        reliableSequence_ = packet.sequence;
        recordSend(nowMs, payload.size());
        outgoing.append({Channel::Reliable, payload});
    }

    // 移动只用剩余名额，且必须给按键留出 kMoveReserveForKeys 个不许碰。
    const bool moveIntervalElapsed =
        lastMoveSentMs_ == 0 || nowMs - lastMoveSentMs_ >= kMoveIntervalMs;
    if (!pendingMovePath_.isEmpty() && moveIntervalElapsed) {
        Packet packet;
        packet.sessionId = sessionId_;
        packet.sequence = unreliableSequence_ + 1;
        for (const auto& point : pendingMovePath_) {
            Event move;
            move.type = EventType::MouseMove;
            move.x = point.x();
            move.y = point.y();
            packet.events.append(move);
        }

        const QByteArray payload = encodePacket(packet);
        if (canSend(nowMs, payload.size(), kBudgetPerSecond - kMoveReserveForKeys)) {
            pendingMovePath_.clear();
            unreliableSequence_ = packet.sequence;
            lastMoveSentMs_ = nowMs;
            recordSend(nowMs, payload.size());
            outgoing.append({Channel::Unreliable, payload});
        }
    }

    return outgoing;
}

void RemoteInputSender::pruneHistory(qint64 nowMs) {
    while (!sendHistory_.isEmpty() && nowMs - sendHistory_.first().first >= kWindowMs) {
        sendHistory_.removeFirst();
    }
}

int RemoteInputSender::windowCount(qint64 nowMs) {
    pruneHistory(nowMs);
    return sendHistory_.size();
}

int RemoteInputSender::windowBytes(qint64 nowMs) {
    pruneHistory(nowMs);
    int total = 0;
    for (const auto& entry : sendHistory_) total += entry.second;
    return total;
}

bool RemoteInputSender::canSend(qint64 nowMs, int payloadBytes, int countLimit) {
    if (windowCount(nowMs) >= countLimit) return false;
    // 字节配额和条数配额是两个独立的闸门，SDK 任一超了都拒发。
    return windowBytes(nowMs) + payloadBytes <= kMaxBytesPerSecond;
}

void RemoteInputSender::recordSend(qint64 nowMs, int payloadBytes) {
    sendHistory_.append(qMakePair(nowMs, payloadBytes));
}

}  // namespace RemoteInput
