#include "model/MessageSearch.h"

#include <QStringList>

namespace MessageSearch {
namespace {

// 子序列命中的跨度上限 = 查询长度的这个倍数。超出就不算命中。
//
// 不设上限时长消息几乎能凑出任何短查询的子序列。在 7340 条真实消息上实测：
// 搜「发版」有 201 条子序列命中，其中 170 条跨度超过查询长度的 8 倍；
// 最极端的一条是 1619 字的文档，「提」在第 15 字、「送」在第 1354 字，中间隔了 1339 字。
// 而结果面板只有 60 个位置，这类噪音会把真正贴切的那几条挤出去。
//
// 取 4 倍而不是 2 倍：中文里「构建那一步失败」这种在词之间插三四个字很常见，
// 收得太紧会把这类真命中也误伤。
constexpr int kSubsequenceSpanFactor = 4;

// needle 的字符按顺序出现在 text 里、且**全部落在一个足够窄的窗口内**。
//
// 注意不能只做一次前向贪心：贪心从最早的起点出发，算出的跨度不一定是最小的。
// 例如正文前半段散着几个巧合字符、后半段才是真正紧凑的那一处，
// 只看贪心结果会把这条误判成「太散」而丢掉。
// 所以每找到一个匹配结束位置，就反向贪心把起点收紧，得到以该位置结尾的最小窗口；
// 若仍超限，则从收紧后的起点之后继续找下一处。
bool isSubsequenceWithinSpan(const QString& text, const QString& needle) {
    const int textSize = text.size();
    const int needleSize = needle.size();
    const int maxSpan = needleSize * kSubsequenceSpanFactor;

    int from = 0;
    while (from < textSize) {
        int cursor = 0;
        int end = -1;
        for (int j = from; j < textSize; ++j) {
            if (text.at(j).toCaseFolded() == needle.at(cursor).toCaseFolded()) {
                if (++cursor == needleSize) { end = j; break; }
            }
        }
        if (end < 0) return false;

        int back = needleSize - 1;
        int start = -1;
        for (int j = end; j >= from; --j) {
            if (text.at(j).toCaseFolded() == needle.at(back).toCaseFolded()) {
                if (back == 0) { start = j; break; }
                --back;
            }
        }
        if (start < 0) return false;
        if (end - start + 1 <= maxSpan) return true;
        from = start + 1;
    }
    return false;
}

}  // namespace

int score(const QString& text, const QString& needle) {
    const QString cleanNeedle = needle.trimmed();
    if (cleanNeedle.isEmpty() || text.isEmpty()) return NoMatch;

    const int at = text.indexOf(cleanNeedle, 0, Qt::CaseInsensitive);
    if (at == 0) return Prefix;
    if (at > 0) return Substring;

    const QStringList tokens = cleanNeedle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() > 1) {
        bool all = true;
        for (const QString& token : tokens) {
            if (!text.contains(token, Qt::CaseInsensitive)) { all = false; break; }
        }
        if (all) return AllTokens;
    }

    // 单字查询不做子序列：一个「的」能命中几乎所有消息，那样的结果没有意义。
    if (cleanNeedle.size() >= 2 && isSubsequenceWithinSpan(text, cleanNeedle)) return Subsequence;
    return NoMatch;
}

bool matches(const QString& text, const QString& needle) {
    return score(text, needle) != NoMatch;
}

QList<int> matchIndexes(const QList<RemoteIMMessage>& messages, const QString& needle) {
    QList<int> hits;
    for (int i = 0; i < messages.size(); ++i) {
        if (matches(messages.at(i).text, needle)) hits.append(i);
    }
    return hits;
}

int nextHit(const QList<int>& hits, int current) {
    if (hits.isEmpty()) return -1;
    for (int hit : hits) {
        if (hit > current) return hit;
    }
    return hits.first();
}

int previousHit(const QList<int>& hits, int current) {
    if (hits.isEmpty()) return -1;
    for (int i = hits.size() - 1; i >= 0; --i) {
        if (hits.at(i) < current) return hits.at(i);
    }
    return hits.last();
}

}  // namespace MessageSearch
