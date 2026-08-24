#include "model/MessageSearch.h"

#include <QStringList>

namespace MessageSearch {
namespace {

// 子序列匹配：needle 的字符按顺序出现在 text 里，允许中间夹别的字符。
bool isSubsequence(const QString& text, const QString& needle) {
    int cursor = 0;
    for (const QChar ch : text) {
        if (ch.toCaseFolded() == needle.at(cursor).toCaseFolded()) {
            if (++cursor == needle.size()) return true;
        }
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
    if (cleanNeedle.size() >= 2 && isSubsequence(text, cleanNeedle)) return Subsequence;
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
