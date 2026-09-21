#include "markdown/MarkdownLayoutCache.h"

#include <QHash>
#include <QList>
#include <QtGlobal>
#include <unordered_map>
#include <vector>

namespace {

// 留多少。两条线，哪条先到都开始丢。
//
// 份数这条防的是「很多条很短的消息」；字数这条防的是「几条超长的消息」——
// 一份的内存大头是 QTextLayout 里排好的字形，大致和字数成正比，按每字 50 字节
// 估，五十万字约合二三十兆，对桌面端是能接受的量级。
//
// 一条消息通常占三份（Qt 的布局会按三个不同宽度问过来），所以一千五百份
// 大约覆盖五百条消息——比一屏多得多，来回切几个会话都够用。
// 丢错了也只是下次重排一遍，不会出错。
constexpr int kMaxEntries = 1500;
constexpr int kMaxCharacters = 500000;

struct Key {
    QString source;
    QString note;
    int notePixelSize = 0;
    unsigned int noteRgba = 0;
    qreal width = 0;
    qreal zoom = 0;

    bool operator==(const Key& other) const {
        return source == other.source && note == other.note
               && notePixelSize == other.notePixelSize && noteRgba == other.noteRgba
               && qFuzzyCompare(width, other.width) && qFuzzyCompare(zoom, other.zoom);
    }
};

struct Entry {
    Key key;
    std::unique_ptr<MarkdownLayout> layout;

    Entry(Key entryKey, std::unique_ptr<MarkdownLayout> entryLayout)
        : key(std::move(entryKey)), layout(std::move(entryLayout)) {}
    // 只能移动：里面装着独占的排版结果。move 要显式标 noexcept，
    // 否则 std::vector 扩容时会退回去用拷贝，而拷贝是删掉的。
    Entry(Entry&&) noexcept = default;
    Entry& operator=(Entry&&) noexcept = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
};

// 哈希只用来找桶，**是不是同一份还得比键**。见头文件里那段。
quint64 hashOf(const Key& key) {
    quint64 hash = 1469598103934665603ull;
    const auto mix = [&hash](quint64 value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(quint64(qHash(key.source)));
    mix(quint64(qHash(key.note)));
    mix(quint64(key.notePixelSize));
    mix(quint64(key.noteRgba));
    // 宽度和缩放都是小数，按 1/16 的精度取整——排版本来也只吃得下这个精度。
    mix(quint64(qRound(key.width * 16)));
    mix(quint64(qRound(key.zoom * 1024)));
    return hash;
}

struct Store {
    // 一个哈希值可能挂着不止一份（撞了，或者内容相同宽度不同）。
    //
    // 容器都得用标准库的：Qt5 的 QList / QHash 都要求元素可拷贝，
    // 而这里存的是只能移动的排版结果。
    std::unordered_map<quint64, std::vector<Entry>> buckets;
    // 最久没用的排在前面。满了从这头丢。
    QList<quint64> order;
    int total = 0;
    int characters = 0;
    int hits = 0;
    int misses = 0;
};

Store& store() {
    static Store instance;
    return instance;
}

void dropOldest() {
    Store& data = store();
    while ((data.total > kMaxEntries || data.characters > kMaxCharacters)
           && !data.order.isEmpty()) {
        const quint64 hash = data.order.takeFirst();
        const auto bucket = data.buckets.find(hash);
        if (bucket == data.buckets.end() || bucket->second.empty()) continue;
        data.characters -= bucket->second.front().key.source.size();
        bucket->second.erase(bucket->second.begin());
        --data.total;
        if (bucket->second.empty()) data.buckets.erase(bucket);
    }
}

}  // namespace

std::unique_ptr<MarkdownLayout> MarkdownLayoutCache::take(const QString& source,
                                                          const QString& note, int notePixelSize,
                                                          unsigned int noteRgba, qreal width,
                                                          qreal zoom) {
    const Key key{source, note, notePixelSize, noteRgba, width, zoom};
    const quint64 hash = hashOf(key);
    Store& data = store();
    const auto bucket = data.buckets.find(hash);
    if (bucket == data.buckets.end()) {
        ++data.misses;
        return nullptr;
    }

    std::vector<Entry>& entries = bucket->second;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!(entries[index].key == key)) continue;
        std::unique_ptr<MarkdownLayout> layout = std::move(entries[index].layout);
        data.characters -= entries[index].key.source.size();
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
        --data.total;
        // order 里对应的那个哈希留着也没关系：dropOldest 见到空桶会跳过。
        if (entries.empty()) data.buckets.erase(bucket);
        ++data.hits;
        return layout;
    }
    ++data.misses;
    return nullptr;
}

void MarkdownLayoutCache::put(const QString& source, const QString& note, int notePixelSize,
                              unsigned int noteRgba, qreal width, qreal zoom,
                              std::unique_ptr<MarkdownLayout> layout) {
    if (layout == nullptr) return;
    // 空内容不值得占位置。
    if (source.isEmpty()) return;

    const Key key{source, note, notePixelSize, noteRgba, width, zoom};
    const quint64 hash = hashOf(key);
    Store& data = store();
    // 选区是部件的状态，不是内容的。留着的话下次取出来会带着别人的高亮。
    layout->clearSelection();
    data.buckets[hash].emplace_back(key, std::move(layout));
    data.order.append(hash);
    ++data.total;
    data.characters += source.size();
    dropOldest();
}

void MarkdownLayoutCache::clear() {
    Store& data = store();
    data.buckets.clear();
    data.order.clear();
    data.total = 0;
    data.characters = 0;
    data.hits = 0;
    data.misses = 0;
}

int MarkdownLayoutCache::count() {
    return store().total;
}

int MarkdownLayoutCache::hits() {
    return store().hits;
}

int MarkdownLayoutCache::misses() {
    return store().misses;
}
