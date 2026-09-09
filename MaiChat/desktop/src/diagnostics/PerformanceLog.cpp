#include "diagnostics/PerformanceLog.h"
#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <cmath>

namespace RemoteDiagnostics {
PerformanceLog& PerformanceLog::shared() { static PerformanceLog log; return log; }
void PerformanceLog::setContext(const QString& context) {
    if (context_ == context) return;
    context_ = context; values_.clear(); dropped_ = 0; previousTick_ = -1;
    startedAt_ = QDateTime::currentMSecsSinceEpoch();
}
void PerformanceLog::record(const QString& metric, double ms, qint64 atMs) {
    static const QSet<QString> allowed{"composer-change", "composer-event-queue", "message-refresh", "message-layout",
        "history-load", "history-write", "image-decode", "main-runloop-delay"};
    if (context_.isEmpty() || !allowed.contains(metric) || !std::isfinite(ms) || ms < 0) return;
    if (atMs < 0) atMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 bucket = atMs / 2000 * 2000;
    Aggregate* aggregate = nullptr;
    for (int i = values_.size() - 1; i >= 0; --i) {
        if (values_[i].bucket == bucket && values_[i].metric == metric) { aggregate = &values_[i]; break; }
        if (values_[i].bucket < bucket) break;
    }
    if (!aggregate) {
        if (values_.size() >= 4096) { values_.removeFirst(); ++dropped_; }
        values_.append(Aggregate{bucket, metric}); aggregate = &values_.last();
    }
    ++aggregate->samples; aggregate->total += ms; aggregate->maximum = qMax(aggregate->maximum, ms);
    if (ms >= (metric == "main-runloop-delay" ? 150.0 : 16.0)) ++aggregate->slow;
}
void PerformanceLog::heartbeat(qint64 monotonicMs, bool active, qint64 atMs) {
    if (!active) { previousTick_ = -1; return; }
    if (previousTick_ >= 0 && monotonicMs >= previousTick_)
        record("main-runloop-delay", qMax<qint64>(0, monotonicMs - previousTick_ - 250), atMs);
    previousTick_ = monotonicMs;
}
QJsonObject PerformanceLog::snapshot(qint64 sinceMs, qint64 nowMs) const {
    QJsonArray events;
    for (const auto& a : values_) {
        if (a.bucket + 2000 <= sinceMs || a.bucket > nowMs) continue;
        events.append(QJsonObject{{"createdAt", double(a.bucket)}, {"event", a.metric}, {"samples", a.samples},
            {"slow_samples", a.slow}, {"slow_threshold_ms", a.metric == "main-runloop-delay" ? 150 : 16},
            {"max_ms", a.maximum}, {"average_ms", a.total / a.samples}});
    }
    QJsonArray supported;
    for (const auto* name : {"composer-change", "composer-event-queue", "message-refresh", "message-layout", "history-load", "history-write", "image-decode", "main-runloop-delay"}) supported.append(QString::fromLatin1(name));
    return {{"supportedMetrics", supported},
        {"firstRetainedBucketAt", events.isEmpty() ? QJsonValue() : events.first().toObject().value("createdAt")},
        {"lastRetainedBucketAt", events.isEmpty() ? QJsonValue() : events.last().toObject().value("createdAt")},
        {"startupBeforeWindow", "unavailable"}, {"status", context_.isEmpty() ? "unavailable" : "ok"}, {"scope", "account-process"},
        {"from", double(sinceMs)}, {"to", double(nowMs)}, {"collectionStartedAt", double(startedAt_)},
        {"bucket_ms", 2000}, {"sample_interval_ms", 250}, {"capacity", 4096},
        {"droppedBuckets", double(dropped_)}, {"truncated", dropped_ > 0}, {"events", events},
        {"eventCount", events.size()}, {"stackTraces", "unavailable"},
        {"interpretation", "Aggregates measure app work and late UI ticks, not physical keypress latency or causal stacks. Empty metrics mean no retained samples."}};
}
}
