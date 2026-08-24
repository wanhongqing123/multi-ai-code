#pragma once

#include <QList>
#include <QString>

#include "model/RemoteIMMessage.h"

// 会话内消息搜索。抽成不依赖控件的纯函数，是因为 UI 部分（滚动、高亮）在这套
// 测试里跑不动，而「哪些消息算命中、命中的先后顺序」才是会出错的地方。
namespace MessageSearch {

// 命中的消息在输入列表中的下标，按输入顺序（即时间先后）排列。
// 空白查询返回空列表——空查询不该把整个会话都算成命中。
// 附件消息的正文是「[图片消息] 文件名」这类占位文本，因此按文件名也能搜到。
QList<int> matchIndexes(const QList<RemoteIMMessage>& messages, const QString& needle);

// 在已排好的命中里，从 current 往后/往前取一个，到头回绕。
// hits 为空时返回 -1。current 不在 hits 里（例如刚改了关键词）时，
// 向后取第一个、向前取最后一个。
int nextHit(const QList<int>& hits, int current);
int previousHit(const QList<int>& hits, int current);

}  // namespace MessageSearch
