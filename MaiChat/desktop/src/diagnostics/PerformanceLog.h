#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <QString>

namespace RemoteDiagnostics {
// UI-thread-only numeric aggregates. No keystrokes, filenames or message bodies.
class PerformanceLog {
public:
    static PerformanceLog& shared();
    QString context() const { return context_; }
    void setContext(const QString& context);
    void record(const QString& metric, double milliseconds, qint64 atMs = -1);
    void heartbeat(qint64 monotonicMs, bool active, qint64 atMs = -1);
    QJsonObject snapshot(qint64 sinceMs, qint64 nowMs) const;
private:
    struct Aggregate { qint64 bucket = 0; QString metric; int samples = 0; int slow = 0; double total = 0; double maximum = 0; };
    QString context_;
    QVector<Aggregate> values_;
    qint64 previousTick_ = -1;
    qint64 dropped_ = 0;
    qint64 startedAt_ = 0;
};
class PerformanceSpan {
public:
    explicit PerformanceSpan(const char* metric) : metric_(QString::fromLatin1(metric)), context_(PerformanceLog::shared().context()) { timer_.start(); }
    ~PerformanceSpan() { if (context_ == PerformanceLog::shared().context()) PerformanceLog::shared().record(metric_, timer_.nsecsElapsed() / 1000000.0); }
private:
    QString metric_;
    QString context_;
    QElapsedTimer timer_;
};
}
