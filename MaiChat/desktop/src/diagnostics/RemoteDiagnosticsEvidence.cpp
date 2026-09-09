#include "diagnostics/RemoteDiagnosticsEvidence.h"

namespace RemoteDiagnostics {

QString describeAttachmentPhase(AttachmentPhase phase)
{
    // 固定文案，本地渲染。不拼接任何来自网络或对端的字符串。
    switch (phase) {
    case AttachmentPhase::MetadataReceived:
        return QStringLiteral("收到附件信息");
    case AttachmentPhase::DownloadStarted:
        return QStringLiteral("开始下载附件");
    case AttachmentPhase::CacheHit:
        return QStringLiteral("命中本地缓存，未重新下载");
    case AttachmentPhase::DownloadFailed:
        return QStringLiteral("附件下载失败");
    case AttachmentPhase::WriteFailed:
        return QStringLiteral("附件下载完成但保存失败");
    case AttachmentPhase::Delivered:
        return QStringLiteral("附件已保存并交付");
    case AttachmentPhase::DroppedForAccountSwitch:
        return QStringLiteral("下载完成时账号已切换，结果未并入会话");
    }
    return QString();
}

EvidenceLog::EvidenceLog(int capacity)
    : capacity_(capacity > 0 ? capacity : 1)
{
}

void EvidenceLog::record(const AttachmentEvent& event)
{
    // 归属不完整就不收。收下来再靠导出过滤，等于给「不小心导出去」留一次机会；
    // 而且一条不知道属于谁的记录，本身对排障也没有价值。
    if (!event.account.isValid() || event.peerId.isEmpty()) return;
    events_.append(event);
    while (events_.size() > capacity_) events_.removeFirst();
}

QList<AttachmentEvent> EvidenceLog::exportFor(const AccountTag& account,
                                              const QString& peerId) const
{
    QList<AttachmentEvent> result;
    if (!account.isValid() || peerId.isEmpty()) return result;
    for (const AttachmentEvent& event : events_) {
        // 账号与好友**两个维度都要匹配**。只按好友筛的话，换账号之后
        // 与同名好友的旧记录会被一起捞出来。
        if (event.account != account) continue;
        if (event.peerId != peerId) continue;
        result.append(event);
    }
    return result;
}

}  // namespace RemoteDiagnostics
