#pragma once

#include <QList>
#include <QString>

// 排障现场记录（A 端本地证据）。
//
// **为什么不直接刮应用日志**：`maichat-<date>.log` 是按天、按进程分的全局文件，
// 不按账号分，而且除了 `[im] connect` 那一行以外，逐条日志都不带账号。
// 同一进程可以 logout 再 login 到另一个账号（`disconnectFromService()` 走
// logout+uninit），文件又保留 7 天——只按好友 ID 筛，会把另一个账号下
// **同名好友**的记录一起捞进来，发给 C。
//
// 所以账号归属必须**记在数据上**，而不是靠流程状态：流程结束了数据还在，
// 下一次排障照样能读到。取消本次任务挡不住这个。
namespace RemoteDiagnostics {

// 账号身份 = SDK 应用号 + 登录用户。同账号断线重连仍是同一个 AccountTag，
// 不会被误判成换账号——这里刻意不掺任何会话/连接序号。
struct AccountTag {
    quint64 sdkAppId = 0;
    QString ownerUserId;

    bool isValid() const { return sdkAppId != 0 && !ownerUserId.isEmpty(); }
    bool operator==(const AccountTag& other) const
    {
        return sdkAppId == other.sdkAppId && ownerUserId == other.ownerUserId;
    }
    bool operator!=(const AccountTag& other) const { return !(*this == other); }
};

// 附件从「知道有这个文件」到「真的交付」之间的固定阶段。
//
// 只记阶段，不记 URL、缓存路径或原始错误文本：那些要么是路径，要么是
// 对端/网络栈可控的自由文本，都不该出现在发给 C 的报告里。
enum class AttachmentPhase {
    MetadataReceived,  // 收到文件元数据（还没开始取）
    DownloadStarted,   // 开始下载
    CacheHit,          // 命中本地缓存，未走网络
    DownloadFailed,    // 网络层失败
    WriteFailed,       // 下载成功但落盘失败（open 失败或写入字节数不足）
    Delivered          // 文件确实落盘且消息已投递
};

struct AttachmentEvent {
    AccountTag account;
    QString peerId;
    QString messageId;
    AttachmentPhase phase = AttachmentPhase::MetadataReceived;
    // 固定错误码（网络栈的 enum 值 / 写入短缺字节数）。不带文本。
    int code = 0;
    qint64 atMs = 0;
};

QString describeAttachmentPhase(AttachmentPhase phase);

// 进程内的现场记录。条目带账号标签，导出时按 账号 + 好友 同时筛。
class EvidenceLog {
public:
    // 上限存在的理由：这是常驻内存的环形记录，不能因为一次长会话就无限增长。
    explicit EvidenceLog(int capacity = 512);

    // 归属不完整（账号无效或没有好友 ID）的条目**直接不收**。
    // 收下来再在导出时过滤，等于给「不小心导出去」留了一次机会。
    void record(const AttachmentEvent& event);

    // 只返回同时匹配账号与好友的条目。任一不匹配都不出现在结果里。
    QList<AttachmentEvent> exportFor(const AccountTag& account, const QString& peerId) const;

    int size() const { return events_.size(); }
    void clear() { events_.clear(); }

private:
    int capacity_;
    QList<AttachmentEvent> events_;
};

}  // namespace RemoteDiagnostics
