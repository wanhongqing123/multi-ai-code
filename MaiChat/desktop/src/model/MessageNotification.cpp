#include "model/MessageNotification.h"

namespace MessageNotification {

namespace {

// 附件消息在没有配文时的占位。用类型而不是文件名：文件名经常就是路径的最后一段，
// 而且带着扩展名和一串随机码，摆在通知里既难看又多余。
QString attachmentPlaceholder(const RemoteIMMessage& message) {
    if (message.hasImage) return QStringLiteral("[图片]");
    if (message.hasVideo) return QStringLiteral("[视频]");
    if (message.hasVoice) return QStringLiteral("[语音]");
    if (message.hasFile) return QStringLiteral("[文件]");
    return QString();
}

// 入站消息的 text 有时是占位串（「[图片消息] xxx」这种），那不是用户写的配文，
// 拿去当预览会把内部约定暴露到通知栏里。
bool looksLikePlaceholderText(const QString& text) {
    return text.startsWith(QStringLiteral("[图片消息] "))
        || text.startsWith(QStringLiteral("[文件消息] "))
        || text.startsWith(QStringLiteral("[视频消息] "))
        || text.startsWith(QStringLiteral("[语音消息] "));
}

QString truncated(const QString& text) {
    const QString clean = text.simplified();
    if (clean.size() <= kPreviewLimit) return clean;
    return clean.left(kPreviewLimit) + QStringLiteral("…");
}

}  // namespace

DeliveryTracker::DeliveryTracker(qint64 sessionStartedAtMillis)
    : sessionStartedAtMillis_(sessionStartedAtMillis) {}

DeliveryDecision DeliveryTracker::record(const QString& peerId, qint64 messageCreatedAtMillis) {
    const QString peer = peerId.trimmed();
    if (peer.isEmpty()) return {DeliveryDisposition::StartupBacklog, 0};

    // 腾讯 IM 的服务端时间通常只有秒级精度。把启动时刻也降到整秒：本进程启动后、
    // 但恰好落在同一秒的新消息不能被误当成历史；更早秒的消息则是登录后的补投。
    const qint64 sessionSecond = (sessionStartedAtMillis_ / 1000) * 1000;
    if (sessionStartedAtMillis_ > 0
        && messageCreatedAtMillis > 0
        && messageCreatedAtMillis < sessionSecond) {
        return {DeliveryDisposition::StartupBacklog, 0};
    }

    const int pending = pendingCounts_.value(peer, 0) + 1;
    pendingCounts_.insert(peer, pending);
    if (pending > 1) return {DeliveryDisposition::AlreadyPending, pending};
    return {DeliveryDisposition::Show, pending};
}

void DeliveryTracker::clear(const QString& peerId) {
    pendingCounts_.remove(peerId.trimmed());
}

void DeliveryTracker::clearAll() {
    pendingCounts_.clear();
}

bool shouldSuppressForForegroundWindow(bool applicationActive, bool isMinimized) {
    return applicationActive && !isMinimized;
}

QString title(const QString& displayName, const QString& peerId) {
    const QString name = displayName.trimmed();
    return name.isEmpty() ? peerId.trimmed() : name;
}

QString preview(const RemoteIMMessage& message) {
    const QString text = message.text.trimmed();
    const QString placeholder = attachmentPlaceholder(message);

    if (!placeholder.isEmpty()) {
        // 有配文就用配文，并在前面标出附件类型——只看见一句话不知道还带了张图。
        if (text.isEmpty() || looksLikePlaceholderText(text)) return placeholder;
        return placeholder + QLatin1Char(' ') + truncated(text);
    }
    if (text.isEmpty()) return QStringLiteral("[新消息]");
    return truncated(text);
}

}  // namespace MessageNotification
