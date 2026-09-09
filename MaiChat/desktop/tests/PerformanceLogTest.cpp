#include <QTest>
#include <QJsonArray>
#include "diagnostics/PerformanceLog.h"
#include <limits>
class PerformanceLogTest : public QObject {
    Q_OBJECT
private slots:
    void aggregatesOnlyFiniteAllowedMetrics() {
        RemoteDiagnostics::PerformanceLog log; log.setContext("account");
        log.record("composer-change", 5, 1000); log.record("composer-change", 25, 1001);
        log.record("composer-change", std::numeric_limits<double>::infinity(), 1001);
        log.record("PRIVATE_BODY", 20, 1001);
        const auto events = log.snapshot(0, 2000)["events"].toArray(); QCOMPARE(events.size(), 1);
        const auto e = events[0].toObject(); QCOMPARE(e["samples"].toInt(), 2);
        QCOMPARE(e["slow_samples"].toInt(), 1); QCOMPARE(e["average_ms"].toDouble(), 15.0);
        QCOMPARE(e["max_ms"].toDouble(), 25.0);
    }
    void inactivityAndAccountSwitchResetHeartbeat() {
        RemoteDiagnostics::PerformanceLog log; log.setContext("a");
        log.heartbeat(0, true, 1000); log.heartbeat(450, true, 1450);
        auto event = log.snapshot(0, 2000)["events"].toArray()[0].toObject();
        QCOMPARE(event["max_ms"].toDouble(), 200.0);
        log.heartbeat(500, false); log.heartbeat(500000, true);
        QCOMPARE(log.snapshot(0, 2000)["events"].toArray().size(), 1);
        log.setContext("b"); QVERIFY(log.snapshot(0, 2000)["events"].toArray().isEmpty());
        log.heartbeat(500250, true); QVERIFY(log.snapshot(0, 2000)["events"].toArray().isEmpty());
    }
    void reportsCapacityLossAndFiltersTime() {
        RemoteDiagnostics::PerformanceLog log; log.setContext("a");
        for (int i = 0; i < 4200; ++i) log.record("history-load", 1, i * 2000);
        auto report = log.snapshot(0, 9000000); QCOMPARE(report["eventCount"].toInt(), 4096);
        QVERIFY(report["truncated"].toBool()); QCOMPARE(report["droppedBuckets"].toInt(), 104);
        QCOMPARE(log.snapshot(8398000, 8399000)["events"].toArray().size(), 1);
    }
};
QTEST_GUILESS_MAIN(PerformanceLogTest)
#include "PerformanceLogTest.moc"
