#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "diagnostics/RemoteDiagnosticsProtocol.h"

using namespace RemoteDiagnostics;

namespace {

const QString kPeer = QStringLiteral("faulty-client");

QByteArray reportBytes(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject minimalReport(const QString& requestId)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    root.insert(QStringLiteral("requestId"), requestId);
    root.insert(QStringLiteral("appVersion"), QStringLiteral("0.1.77"));
    root.insert(QStringLiteral("platform"), QStringLiteral("win32"));
    return root;
}

}  // namespace

class RemoteDiagnosticsProtocolTest : public QObject {
    Q_OBJECT

private slots:
    void generatedRequestIdIsAcceptedByOurOwnValidator();
    void rejectsRequestIdShapesThatWouldNotCompareEqual();
    void commandRoundTripsAndIgnoresOtherText();
    void rejectsAReportFromSomeoneOtherThanTheFaultyClient();
    void rejectsAReportWhoseIdMatchesOnlyTheFileName();
    void rejectsAnOversizedReport();
    void keepsWhitelistedIdentifiersIncludingTheUppercaseSdkId();
    void dropsUnknownFieldsInsteadOfPassingThemThrough();
    void keepsOnDiskBinaryMetadataWithItsCorrectTypes();
    void keepsOnlyTheTwoKnownCoverageValues();
};

// requestId 由我们生成、由我们校验，两边形状必须自洽。
void RemoteDiagnosticsProtocolTest::generatedRequestIdIsAcceptedByOurOwnValidator()
{
    for (int i = 0; i < 32; ++i) {
        const QString id = newRequestId();
        QVERIFY2(isValidRequestId(id), qPrintable(QStringLiteral("rejected own id: %1").arg(id)));
        QCOMPARE(id, id.toLower());
    }
}

// 两端按字符串逐字符比对，放宽形状就会出现「看起来一样但不相等」。
void RemoteDiagnosticsProtocolTest::rejectsRequestIdShapesThatWouldNotCompareEqual()
{
    const QString valid = newRequestId();
    QVERIFY(!isValidRequestId(valid.toUpper()));
    QVERIFY(!isValidRequestId(QStringLiteral("{%1}").arg(valid)));
    QVERIFY(!isValidRequestId(valid + QStringLiteral(" ")));
    QVERIFY(!isValidRequestId(QString()));
    QVERIFY(!isValidRequestId(QStringLiteral("not-a-uuid")));
}

void RemoteDiagnosticsProtocolTest::commandRoundTripsAndIgnoresOtherText()
{
    const QString id = newRequestId();
    QCOMPARE(parseCommand(formatCommand(id)), id);
    // 用户正常聊天里出现的相似文本不能被当成命令。
    QVERIFY(parseCommand(QStringLiteral("/diagnostics")).isEmpty());
    QVERIFY(parseCommand(QStringLiteral("/diagnostics not-a-uuid")).isEmpty());
    QVERIFY(parseCommand(QStringLiteral("聊聊 /diagnostics ") + id).isEmpty());
}

// 伪造报告不能因为「格式正确」就被并进去。
void RemoteDiagnosticsProtocolTest::rejectsAReportFromSomeoneOtherThanTheFaultyClient()
{
    const QString id = newRequestId();
    const ParsedReport parsed =
        parseReport(reportBytes(minimalReport(id)), attachmentFileName(id), id,
                    QStringLiteral("someone-else"), kPeer);
    QVERIFY(!parsed.accepted);
    QVERIFY(!parsed.rejectionReason.isEmpty());
    QVERIFY(parsed.report.isEmpty());
}

// 文件名和 JSON 里的编号都要查：只查一个就能被「文件名对、内容是别人的」绕过。
void RemoteDiagnosticsProtocolTest::rejectsAReportWhoseIdMatchesOnlyTheFileName()
{
    const QString id = newRequestId();
    QJsonObject body = minimalReport(newRequestId());  // 内容里是另一个编号
    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(!parsed.accepted);

    // 反过来同样不接受：内容对、文件名不对。
    const ParsedReport swapped =
        parseReport(reportBytes(minimalReport(id)),
                    attachmentFileName(newRequestId()), id, kPeer, kPeer);
    QVERIFY(!swapped.accepted);
}

void RemoteDiagnosticsProtocolTest::rejectsAnOversizedReport()
{
    const QString id = newRequestId();
    QJsonObject body = minimalReport(id);
    body.insert(QStringLiteral("appVersion"), QString(kMaxReportBytes, QLatin1Char('x')));
    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(!parsed.accepted);
    QVERIFY(parsed.rejectionReason.contains(QStringLiteral("上限")));
}

// 大写字符串 ID（来自 send:created）与数字 messageId 是两个东西。
// 混用或漏掉任一个，两端日志就对不上号——报告再全也接不起来。
void RemoteDiagnosticsProtocolTest::keepsWhitelistedIdentifiersIncludingTheUppercaseSdkId()
{
    const QString id = newRequestId();
    QJsonObject body = minimalReport(id);
    QJsonObject event{{QStringLiteral("ID"), QStringLiteral("144115-ABCDEF")},
                      {QStringLiteral("messageId"), 4321},
                      {QStringLiteral("callId"), QStringLiteral("call-async-question")},
                      {QStringLiteral("phase"), QStringLiteral("final_answer")},
                      {QStringLiteral("delivery"), QStringLiteral("async")},
                      {QStringLiteral("threadId"), QStringLiteral("thread-7")},
                      {QStringLiteral("sdkReady"), true},
                      {QStringLiteral("attempt"), 2}};
    QJsonObject file{{QStringLiteral("source"), QStringLiteral("codex-original-events")},
                     {QStringLiteral("status"), QStringLiteral("ok")},
                     {QStringLiteral("events"), QJsonArray{event}}};
    body.insert(QStringLiteral("files"), QJsonArray{file});

    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(parsed.accepted);

    const QJsonObject kept = parsed.report.value(QStringLiteral("files"))
                                 .toArray()
                                 .at(0)
                                 .toObject()
                                 .value(QStringLiteral("events"))
                                 .toArray()
                                 .at(0)
                                 .toObject();
    QCOMPARE(kept.value(QStringLiteral("ID")).toString(), QStringLiteral("144115-ABCDEF"));
    QCOMPARE(kept.value(QStringLiteral("messageId")).toInt(), 4321);
    QCOMPARE(kept.value(QStringLiteral("callId")).toString(),
             QStringLiteral("call-async-question"));
    QCOMPARE(kept.value(QStringLiteral("phase")).toString(), QStringLiteral("final_answer"));
    QCOMPARE(kept.value(QStringLiteral("delivery")).toString(), QStringLiteral("async"));
    QVERIFY(kept.value(QStringLiteral("sdkReady")).toBool());
    QCOMPARE(kept.value(QStringLiteral("attempt")).toInt(), 2);
}

// 未知字段原样透传，等于把对端可控的内容塞进发给 C 的报告里。
void RemoteDiagnosticsProtocolTest::dropsUnknownFieldsInsteadOfPassingThemThrough()
{
    const QString id = newRequestId();
    QJsonObject body = minimalReport(id);
    body.insert(QStringLiteral("absoluteFilePath"), QStringLiteral("C:/secret/path.log"));
    QJsonObject event{{QStringLiteral("ID"), QStringLiteral("KEEP-ME")},
                      {QStringLiteral("messageBody"), QStringLiteral("私聊正文不该出现")},
                      // 类型不对同样丢弃：拿去比对只会得出错误结论。
                      {QStringLiteral("attempt"), QStringLiteral("2")}};
    body.insert(QStringLiteral("files"),
                QJsonArray{QJsonObject{{QStringLiteral("source"), QStringLiteral("host-log")},
                                       {QStringLiteral("events"), QJsonArray{event}}}});

    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(parsed.accepted);

    const QByteArray serialized = QJsonDocument(parsed.report).toJson(QJsonDocument::Compact);
    QVERIFY(!serialized.contains("absoluteFilePath"));
    QVERIFY(!serialized.contains("messageBody"));
    QVERIFY(!serialized.contains("C:/secret/path.log"));

    const QJsonObject kept = parsed.report.value(QStringLiteral("files"))
                                 .toArray().at(0).toObject()
                                 .value(QStringLiteral("events")).toArray().at(0).toObject();
    QCOMPARE(kept.value(QStringLiteral("ID")).toString(), QStringLiteral("KEEP-ME"));
    QVERIFY(!kept.contains(QStringLiteral("attempt")));
    // 丢了什么要留痕，报告里要能说明「对端给了但我们没收」。
    QVERIFY(!parsed.droppedFields.isEmpty());
}

void RemoteDiagnosticsProtocolTest::keepsOnDiskBinaryMetadataWithItsCorrectTypes()
{
    const QString id = newRequestId();
    QJsonObject body = minimalReport(id);
    QJsonObject session{
        {QStringLiteral("appVersion"), QStringLiteral("0.1.77")},
        {QStringLiteral("sourceCommit"), QStringLiteral("fc26360")},
        {QStringLiteral("pid"), 4242},
        {QStringLiteral("onDiskBinarySha256"), QStringLiteral("ed77f6a4cb2158bd")},
        {QStringLiteral("onDiskBinaryBytes"), 298307584},
        {QStringLiteral("onDiskBinaryStatus"), QStringLiteral("hashed")},
        {QStringLiteral("executablePath"), QStringLiteral("D:/should/not/survive.exe")}};
    body.insert(QStringLiteral("activeSessions"), QJsonArray{session});

    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(parsed.accepted);

    const QJsonObject kept =
        parsed.report.value(QStringLiteral("activeSessions")).toArray().at(0).toObject();
    QCOMPARE(kept.value(QStringLiteral("onDiskBinarySha256")).toString(),
             QStringLiteral("ed77f6a4cb2158bd"));
    QCOMPARE(kept.value(QStringLiteral("onDiskBinaryBytes")).toDouble(), 298307584.0);
    QCOMPARE(kept.value(QStringLiteral("onDiskBinaryStatus")).toString(),
             QStringLiteral("hashed"));
    QVERIFY(!kept.contains(QStringLiteral("executablePath")));
}

// 这个字段的作用正是区分「采到了」和「没采到」。放进第三种取值，
// 就等于让报告显得比它实际知道的更确定。
void RemoteDiagnosticsProtocolTest::keepsOnlyTheTwoKnownCoverageValues()
{
    const QString id = newRequestId();
    for (const QString& value : {QStringLiteral("see-codex-original-events"),
                                 QStringLiteral("unavailable")}) {
        QJsonObject body = minimalReport(id);
        body.insert(QStringLiteral("sourceCoverage"),
                    QJsonObject{{QStringLiteral("aicliOriginalEvents"), value}});
        const ParsedReport parsed =
            parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
        QVERIFY(parsed.accepted);
        QCOMPARE(parsed.report.value(QStringLiteral("sourceCoverage"))
                     .toObject()
                     .value(QStringLiteral("aicliOriginalEvents"))
                     .toString(),
                 value);
    }

    QJsonObject body = minimalReport(id);
    body.insert(QStringLiteral("sourceCoverage"),
                QJsonObject{{QStringLiteral("aicliOriginalEvents"),
                             QStringLiteral("partially-collected")}});
    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(parsed.accepted);
    QVERIFY(!parsed.report.value(QStringLiteral("sourceCoverage"))
                 .toObject()
                 .contains(QStringLiteral("aicliOriginalEvents")));
    QVERIFY(parsed.droppedFields.contains(QStringLiteral("sourceCoverage.aicliOriginalEvents")));
}

QTEST_MAIN(RemoteDiagnosticsProtocolTest)
#include "RemoteDiagnosticsProtocolTest.moc"
