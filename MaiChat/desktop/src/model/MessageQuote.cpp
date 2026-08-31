#include "model/MessageQuote.h"

#include <QFileInfo>

namespace MessageQuote {

namespace {

// 入站附件消息的 text 有时是内部占位串（「[图片消息] xxx」这种），那不是用户写的配文。
// 拿它当摘要会把内部约定漏进引用块里。判据与 MessageNotification 保持一致。
bool looksLikePlaceholderText(const QString& text) {
    return text.startsWith(QStringLiteral("[图片消息] "))
        || text.startsWith(QStringLiteral("[文件消息] "))
        || text.startsWith(QStringLiteral("[视频消息] "))
        || text.startsWith(QStringLiteral("[语音消息] "));
}

// 用户真正写的配文；没有就返回空串。
QString authoredCaption(const RemoteIMMessage& message) {
    const QString text = message.text.trimmed();
    if (text.isEmpty() || looksLikePlaceholderText(text)) return QString();
    return text;
}

// simplified() 正好是「去首尾空白 + 连续空白折成单个空格」，换行也一并折掉：
// 引用块是单行展示，原文里的换行带进来会把布局撑开。
QString clamped(const QString& text) {
    const QString clean = text.simplified();
    if (clean.size() <= kDigestLimit) return clean;
    return clean.left(kDigestLimit) + QStringLiteral("…");
}

}  // namespace

QString kindOf(const RemoteIMMessage& message) {
    if (message.hasImage) return QStringLiteral("image");
    if (message.hasVideo) return QStringLiteral("video");
    if (message.hasVoice) return QStringLiteral("voice");
    if (message.hasFile) return QStringLiteral("file");
    return QStringLiteral("text");
}

QString digestOf(const RemoteIMMessage& message) {
    const QString caption = authoredCaption(message);
    if (message.hasImage) {
        return clamped(caption.isEmpty() ? QStringLiteral("[图片]") : caption);
    }
    if (message.hasVideo) {
        return clamped(caption.isEmpty() ? QStringLiteral("[视频]") : caption);
    }
    if (message.hasVoice) {
        return clamped(caption.isEmpty() ? QStringLiteral("[语音]") : caption);
    }
    if (message.hasFile) {
        if (!caption.isEmpty()) return clamped(caption);
        // 文件名只取 basename。file.name 一般已经是文件名，但它也可能被写成路径，
        // 而把本地路径放进要发给对端的消息里是不能接受的。
        const QString name = QFileInfo(message.file.fileName.isEmpty() ? message.file.localPath
                                                                      : message.file.fileName)
                                 .fileName()
                                 .trimmed();
        return clamped(name.isEmpty() ? QStringLiteral("[文件]")
                                      : QStringLiteral("[文件] ") + name);
    }
    return clamped(caption);
}

QString sdkMessageIdOf(const QString& localMessageId) {
    const int hash = localMessageId.lastIndexOf(QLatin1Char('#'));
    // 只认「末尾是 #<十进制数字>」这一种形状。按第一个 # 截断是不行的：
    // 协议层不该偷偷假设 SDK id 自身永远不含 #。
    if (hash <= 0 || hash == localMessageId.size() - 1) return QString();
    const QStringView suffix = QStringView(localMessageId).mid(hash + 1);
    for (const QChar c : suffix) {
        if (!c.isDigit()) return QString();
    }
    return localMessageId.left(hash);
}

RemoteIMQuote quoteFor(const RemoteIMMessage& message) {
    RemoteIMQuote quote;
    // 拿不到 SDK 派生 ID 就留空。随机 UUID 跨端无法解析，
    // 发出去只会让对端拿到一个永远命中不了的 ID。
    quote.msgId = sdkMessageIdOf(message.id);
    quote.senderId = message.fromUserId;
    quote.digest = digestOf(message);
    quote.kind = kindOf(message);
    return quote;
}

}  // namespace MessageQuote
