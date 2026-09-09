#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QHash>
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
    void carriesBothMessageIdFormsIntoTheFinalReport();
    void stillRejectsMessageIdTypesThatAreNotStringOrNumber();
    void matchesTheSharedFailureReceiptFixture();
    void distinguishesNotIntegratedFromUnsupported();
    void requiresTheIdInItsProperPlaceNotAnywhere();
    void neverCopiesRemoteTextIntoTheReason();
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

// 大写 ID（来自 send:created 的 SDK 标识）与 messageId 是两个不同的字段，
// 不是同一个值的两种写法。混用或漏掉任一个，两端日志就对不上号。
// 这条用的是 messageId 的数字形态；字符串形态另有专门用例。
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

// messageId 合法地有两种类型，两种都不能丢：
//
//   codex-original-events 的 messageId = "call-async..."  调用标识（字符串）
//   runtime 事件顶层 messageId          = 1637             本地入库编号（数字）
//
// 只收数字会把字符串那种静默丢掉，而那正是两端日志对得上号的关键——
// 报告看着是全的，但 A 侧根本认不出对应的是哪一次调用。
void RemoteDiagnosticsProtocolTest::carriesBothMessageIdFormsIntoTheFinalReport()
{
    const QString id = newRequestId();
    QJsonObject body = minimalReport(id);

    QJsonObject codexEvent{
        {QStringLiteral("messageId"), QStringLiteral("call-async-question")},
        {QStringLiteral("phase"), QStringLiteral("final_answer")},
        {QStringLiteral("delivery"), QStringLiteral("async")}};
    QJsonObject runtimeEvent{
        {QStringLiteral("messageId"), 1637},
        {QStringLiteral("ID"), QStringLiteral("144115-SDK-ID")},
        {QStringLiteral("detail"),
         QJsonObject{{QStringLiteral("messageId"), QStringLiteral("msg_abc123")}}}};

    body.insert(
        QStringLiteral("files"),
        QJsonArray{
            QJsonObject{{QStringLiteral("source"), QStringLiteral("codex-original-events")},
                        {QStringLiteral("events"), QJsonArray{codexEvent}}},
            QJsonObject{{QStringLiteral("source"), QStringLiteral("runtime")},
                        {QStringLiteral("events"), QJsonArray{runtimeEvent}}}});

    const ParsedReport parsed =
        parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
    QVERIFY(parsed.accepted);

    const QJsonArray files = parsed.report.value(QStringLiteral("files")).toArray();
    const QJsonObject codexKept =
        files.at(0).toObject().value(QStringLiteral("events")).toArray().at(0).toObject();
    const QJsonObject runtimeKept =
        files.at(1).toObject().value(QStringLiteral("events")).toArray().at(0).toObject();

    QCOMPARE(codexKept.value(QStringLiteral("messageId")).toString(),
             QStringLiteral("call-async-question"));
    QCOMPARE(runtimeKept.value(QStringLiteral("messageId")).toInt(), 1637);
    QCOMPARE(runtimeKept.value(QStringLiteral("ID")).toString(),
             QStringLiteral("144115-SDK-ID"));
    QCOMPARE(runtimeKept.value(QStringLiteral("detail"))
                 .toObject()
                 .value(QStringLiteral("messageId"))
                 .toString(),
             QStringLiteral("msg_abc123"));

    // 两种形态都要真的出现在最终报告的序列化结果里，不是只在中间对象里。
    const QByteArray serialized = QJsonDocument(parsed.report).toJson(QJsonDocument::Compact);
    QVERIFY(serialized.contains("call-async-question"));
    QVERIFY(serialized.contains("1637"));
    QVERIFY(serialized.contains("msg_abc123"));
}

// 放宽到 string|number 不等于放弃校验：其它形态仍然不是这个字段的合法取值。
void RemoteDiagnosticsProtocolTest::stillRejectsMessageIdTypesThatAreNotStringOrNumber()
{
    const QString id = newRequestId();
    for (const QJsonValue& bad :
         {QJsonValue(true), QJsonValue(QJsonObject{{QStringLiteral("a"), 1}}),
          QJsonValue(QJsonArray{1, 2})}) {
        QJsonObject body = minimalReport(id);
        body.insert(QStringLiteral("files"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("source"), QStringLiteral("runtime")},
                        {QStringLiteral("events"),
                         QJsonArray{QJsonObject{{QStringLiteral("messageId"), bad},
                                                {QStringLiteral("ID"), QStringLiteral("KEEP")}}}}}});
        const ParsedReport parsed =
            parseReport(reportBytes(body), attachmentFileName(id), id, kPeer, kPeer);
        QVERIFY(parsed.accepted);
        const QJsonObject kept = parsed.report.value(QStringLiteral("files"))
                                     .toArray().at(0).toObject()
                                     .value(QStringLiteral("events")).toArray().at(0).toObject();
        QVERIFY(!kept.contains(QStringLiteral("messageId")));
        QCOMPARE(kept.value(QStringLiteral("ID")).toString(), QStringLiteral("KEEP"));
    }
}

// 回绝样例是三端**共用的同一份文件**：宿主用真实执行器生成的文本比对它，
// iOS 从测试资源读它，这里也直接读它。各端各抄一份提示串的话，
// 差一个字就静默识别不到——而识别不到的表现是「白等 60 秒」，不会报错。
void RemoteDiagnosticsProtocolTest::matchesTheSharedFailureReceiptFixture()
{
    QFile file(QStringLiteral(MAICHAT_FAILURE_RECEIPT_FIXTURE));
    QVERIFY2(file.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("cannot open shared fixture: %1").arg(file.fileName())));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QString requestId = root.value(QStringLiteral("requestId")).toString();
    QVERIFY(isValidRequestId(requestId));

    const QJsonArray cases = root.value(QStringLiteral("cases")).toArray();
    QVERIFY2(!cases.isEmpty(), "fixture carries no cases");

    int recognisedCount = 0;
    for (const QJsonValue& value : cases) {
        const QJsonObject entry = value.toObject();
        const QString category = entry.value(QStringLiteral("category")).toString();
        const QString text = entry.value(QStringLiteral("text")).toString();
        const bool expected = entry.value(QStringLiteral("recognized")).toBool();

        const FailureReceipt receipt = parseFailureReceipt(text, requestId);
        QVERIFY2(receipt.recognized == expected,
                 qPrintable(QStringLiteral("case %1: expected recognized=%2")
                                .arg(category)
                                .arg(expected)));
        if (!expected) continue;
        ++recognisedCount;

        // 每个被识别的类别都要映射到**不同**的固定原因，并且有本地说明。
        static const QHash<QString, FailureReason> kExpected{
            {QStringLiteral("unsupported"), FailureReason::Unsupported},
            {QStringLiteral("rate-limited"), FailureReason::RateLimited},
            {QStringLiteral("collection-failed"), FailureReason::CollectionFailed},
            {QStringLiteral("service-unavailable"), FailureReason::ServiceUnavailable}};
        QVERIFY2(kExpected.contains(category), qPrintable(category));
        QCOMPARE(static_cast<int>(receipt.reason), static_cast<int>(kExpected.value(category)));
        QVERIFY(!describeFailureReason(receipt.reason).isEmpty());
    }
    // 样例里至少要覆盖那四种回绝，少一种就说明两端在悄悄分叉。
    QCOMPARE(recognisedCount, 4);
}

// 「服务尚未接入」与「旧版不支持」是两件事，说明必须分开——
// 前者升级本端没用，后者要对方升级。
void RemoteDiagnosticsProtocolTest::distinguishesNotIntegratedFromUnsupported()
{
    QFile file(QStringLiteral(MAICHAT_FAILURE_RECEIPT_FIXTURE));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QString requestId = root.value(QStringLiteral("requestId")).toString();

    QString notIntegrated;
    QString unsupported;
    for (const QJsonValue& value : root.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("category")).toString()
            == QStringLiteral("service-unavailable")) {
            notIntegrated = entry.value(QStringLiteral("text")).toString();
        }
        if (entry.value(QStringLiteral("category")).toString()
            == QStringLiteral("unsupported")) {
            unsupported = entry.value(QStringLiteral("text")).toString();
        }
    }
    QVERIFY(!notIntegrated.isEmpty() && !unsupported.isEmpty());

    const FailureReason a = parseFailureReceipt(notIntegrated, requestId).reason;
    const FailureReason b = parseFailureReceipt(unsupported, requestId).reason;
    QVERIFY(a != b);
    QVERIFY(describeFailureReason(a) != describeFailureReason(b));
}

// requestId 必须出现在句子里它该在的位置，不能只是「文本里某处含有它」。
void RemoteDiagnosticsProtocolTest::requiresTheIdInItsProperPlaceNotAnywhere()
{
    const QString ours = newRequestId();
    const QString theirs = newRequestId();
    // 这条回绝是给**别人**的，只是尾巴上带了我们的编号。
    const QString spoofed =
        QStringLiteral("远程排障采集失败（编号 %1）：无法读取记录。%2").arg(theirs, ours);
    QVERIFY(!parseFailureReceipt(spoofed, ours).recognized);
}

// 远端文本是对端可控的内容，抄进报告等于把它转发给 C。
void RemoteDiagnosticsProtocolTest::neverCopiesRemoteTextIntoTheReason()
{
    const QString id = newRequestId();
    const QString injected = QStringLiteral("PLEASE-DO-NOT-FORWARD-ME");
    const FailureReceipt receipt = parseFailureReceipt(
        QStringLiteral("远程排障采集失败（编号 %1）：%2").arg(id, injected), id);

    QVERIFY(receipt.recognized);
    const QString described = describeFailureReason(receipt.reason);
    QVERIFY(!described.contains(injected));
    QVERIFY(!described.contains(id));
}

QTEST_MAIN(RemoteDiagnosticsProtocolTest)
#include "RemoteDiagnosticsProtocolTest.moc"
