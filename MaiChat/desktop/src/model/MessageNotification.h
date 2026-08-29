#pragma once

#include <QString>

#include "model/RemoteIMMessage.h"

// 新消息通知的标题与正文。
//
// 单独拎出来是因为它有一条硬要求：**通知正文里不能出现本地文件路径**。
// 系统通知会出现在锁屏、通知中心、甚至录屏里，路径既没用又泄露目录结构。
namespace MessageNotification {

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
