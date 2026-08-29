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

QString aggregatedPreview(const RemoteIMMessage& latest, int pendingCount) {
    if (pendingCount <= 1) return preview(latest);
    // 只报条数 + 最新一条。逐条弹窗在收到一串消息时会把桌面刷满，
    // 而用户真正想知道的是「谁找我、有几条」。
    return QStringLiteral("%1 条新消息：%2").arg(pendingCount).arg(preview(latest));
}

}  // namespace MessageNotification
