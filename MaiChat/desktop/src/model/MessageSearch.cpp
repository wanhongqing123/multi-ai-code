#include "model/MessageSearch.h"

namespace MessageSearch {

QList<int> matchIndexes(const QList<RemoteIMMessage>& messages, const QString& needle) {
    const QString cleanNeedle = needle.trimmed();
    QList<int> hits;
    if (cleanNeedle.isEmpty()) return hits;
    for (int i = 0; i < messages.size(); ++i) {
        if (messages.at(i).text.contains(cleanNeedle, Qt::CaseInsensitive)) hits.append(i);
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
