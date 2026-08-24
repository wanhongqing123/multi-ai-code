#pragma once

#include <QList>
#include <QString>

#include "model/RemoteIMMessage.h"

// 会话消息搜索。抽成不依赖控件的纯函数，是因为 UI 那部分（滚动、高亮）在这套
// 测试里跑不动，而「哪些算命中、谁排前面」才是会出错的地方。
namespace MessageSearch {

// 匹配分级。严格的子串匹配太脆：记岔一个字、词序记反，就什么都搜不出来。
// 所以按「越像越靠前」分三档，全部算命中，靠排序把最贴切的顶上去。
enum MatchScore {
    NoMatch = 0,
    // 字按顺序出现但中间有别的字：「构建失败」能命中「构建那一步失败了」。
    Subsequence = 30,
    // 空格分词后每个词都出现，顺序不限：「失败 cmake」能命中「cmake 那步失败了」。
    AllTokens = 60,
    // 原样出现。
    Substring = 100,
    // 原样出现且在开头，通常正是想找的那条。
    Prefix = 130
};

// 0 表示不匹配；分数越高越贴切。大小写不敏感。
int score(const QString& text, const QString& needle);

// 是否命中。逐条流式判断时用它，避免为了搜索先把整个会话复制一份出来——
// 会话上千条时，那份拷贝发生在每次按键上，输入就会发涩。
bool matches(const QString& text, const QString& needle);

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
