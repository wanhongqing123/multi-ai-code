#pragma once

#include <QString>
#include <QTextBoundaryFinder>

namespace PreviewText {

inline uint scalarAt(const QString& text, int offset) {
    if (offset < 0 || offset >= text.size()) return 0;
    const QChar first = text.at(offset);
    if (first.isHighSurrogate() && offset + 1 < text.size() && text.at(offset + 1).isLowSurrogate()) {
        return QChar::surrogateToUcs4(first, text.at(offset + 1));
    }
    return first.unicode();
}

inline uint scalarBefore(const QString& text, int offset) {
    if (offset <= 0) return 0;
    int start = offset - 1;
    if (text.at(start).isLowSurrogate() && start > 0 && text.at(start - 1).isHighSurrogate()) --start;
    return scalarAt(text, start);
}

// Older Qt builds split modern emoji sequences into several graphemes. Merge
// these extra boundaries conservatively; do not replace Qt's general Unicode
// segmentation or infer boundaries from how a particular font draws the text.
inline bool continuesEmojiOrMark(const QString& text, int boundary) {
    if (boundary <= 0 || boundary >= text.size()) return false;
    const uint before = scalarBefore(text, boundary);
    const uint after = scalarAt(text, boundary);
    if (text.at(boundary).isLowSurrogate() && text.at(boundary - 1).isHighSurrogate()) return true;
    if (before == 0x200D || after == 0x200D) return true;
    if ((after >= 0xFE00 && after <= 0xFE0F) || (after >= 0xE0100 && after <= 0xE01EF)
        || (after >= 0x1F3FB && after <= 0x1F3FF) || (after >= 0xE0020 && after <= 0xE007F)) return true;
    const auto category = QChar::category(after);
    if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing) return true;
    const auto regional = [](uint value) { return value >= 0x1F1E6 && value <= 0x1F1FF; };
    if (regional(before) && regional(after)) {
        int count = 0;
        for (int offset = boundary; offset > 0;) {
            const uint value = scalarBefore(text, offset);
            if (!regional(value)) break;
            ++count;
            offset -= value > 0xFFFF ? 2 : 1;
        }
        return count % 2 == 1; // Keep each regional-indicator pair together, not all flags.
    }
    return false;
}

inline QString truncate(const QString& text, int limit = 160) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int end = 0;
    for (int count = 0; count < qMax(0, limit); ++count) {
        int next;
        do {
            next = finder.toNextBoundary();
            if (next < 0) return text;
        } while (continuesEmojiOrMark(text, next));
        end = next;
    }
    return end < text.size() ? text.left(end) + QStringLiteral("…") : text;
}

} // namespace PreviewText
