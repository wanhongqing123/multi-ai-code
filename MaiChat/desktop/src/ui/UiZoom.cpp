#include "ui/UiZoom.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>

namespace {

constexpr qreal kMinFactor = 0.8;
constexpr qreal kMaxFactor = 2.0;
constexpr qreal kStep = 0.1;
constexpr qreal kDefaultFactor = 1.0;

bool loaded = false;
qreal current = kDefaultFactor;

qreal clampFactor(qreal value) {
    return qBound(kMinFactor, value, kMaxFactor);
}

bool canPersist() {
    // 组织名未设置（单测环境）时不读写 QSettings，避免测试污染注册表。
    return !QCoreApplication::organizationName().isEmpty();
}

void ensureLoaded() {
    if (loaded) return;
    loaded = true;
    if (!canPersist()) return;
    QSettings settings;
    current = clampFactor(settings.value(QStringLiteral("ui/zoomFactor"), kDefaultFactor).toReal());
}

}  // namespace

namespace UiZoom {

qreal factor() {
    ensureLoaded();
    return current;
}

qreal setFactor(qreal value) {
    ensureLoaded();
    current = clampFactor(value);
    if (canPersist()) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/zoomFactor"), current);
    }
    return current;
}

qreal minFactor() { return kMinFactor; }
qreal maxFactor() { return kMaxFactor; }
qreal step() { return kStep; }

int s(int px) {
    if (px == 0) return 0;
    const int scaled = qRound(px * factor());
    return px > 0 ? qMax(1, scaled) : qMin(-1, scaled);
}

QString scaleQss(const QString& qss) {
    ensureLoaded();
    if (qFuzzyCompare(current, kDefaultFactor)) return qss;
    static const QRegularExpression pxPattern(QStringLiteral("(\\d+(?:\\.\\d+)?)px"));
    QString result;
    result.reserve(qss.size() + 16);
    int last = 0;
    QRegularExpressionMatchIterator it = pxPattern.globalMatch(qss);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += qss.mid(last, match.capturedStart() - last);
        const qreal value = match.captured(1).toDouble();
        const int scaled = qFuzzyIsNull(value) ? 0 : qMax(1, qRound(value * current));
        result += QString::number(scaled);
        result += QStringLiteral("px");
        last = match.capturedEnd();
    }
    result += qss.mid(last);
    return result;
}

}  // namespace UiZoom
