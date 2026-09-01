#pragma once

#include <QHash>
#include <QString>

#include "model/RemoteIMMessage.h"

// 新消息通知的标题与正文。
//
// 单独拎出来是因为它有一条硬要求：**通知正文里不能出现本地文件路径**。
// 系统通知会出现在锁屏、通知中心、甚至录屏里，路径既没用又泄露目录结构。
namespace MessageNotification {

enum class DeliveryDisposition {
    Show,
    StartupBacklog,
    AlreadyPending
};

struct DeliveryDecision {
    DeliveryDisposition disposition = DeliveryDisposition::StartupBacklog;
    int pendingCount = 0;

    bool shouldShow() const { return disposition == DeliveryDisposition::Show; }
};

// 系统托盘通知的交付闸门：
// 1. 本次进程启动前产生、登录后才补投的旧消息只进未读，不弹系统通知；
// 2. 同一联系人尚未被查看时最多弹一次，后续只累计 pendingCount；
// 3. clear 后下一条实时消息可以再次提醒。
class DeliveryTracker {
public:
    explicit DeliveryTracker(qint64 sessionStartedAtMillis);

    DeliveryDecision record(const QString& peerId, qint64 messageCreatedAtMillis);
    void clear(const QString& peerId);
    void clearAll();

private:
    qint64 sessionStartedAtMillis_ = 0;
    QHash<QString, int> pendingCounts_;
};

// 系统通知只在窗口最小化或失去前台焦点时有意义。应用就在用户眼前时，
// 无论当前是哪一页/哪个会话，都不再额外弹 Windows 通知。
bool shouldSuppressForForegroundWindow(bool applicationActive, bool isMinimized);

// 标题用联系人显示名；没有显示名就退回 userId——总得让人知道是谁发的。
QString title(const QString& displayName, const QString& peerId);

// 正文预览。带附件的消息优先用配文，没有配文就用类型占位（「[图片]」这类），
// 永远不用文件名或路径。文本超过 kPreviewLimit 个字符会截断。
QString preview(const RemoteIMMessage& message);

// 同一个人堆了多条时，用聚合正文代替逐条弹窗。
QString aggregatedPreview(const RemoteIMMessage& latest, int pendingCount);

// 预览的最大长度。通知栏本来就显示不下更多，截断处加省略号。
constexpr int kPreviewLimit = 60;

}  // namespace MessageNotification
