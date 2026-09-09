#include <QtTest>

#include <QSet>

#include "diagnostics/RemoteDiagnosticsEvidence.h"

using namespace RemoteDiagnostics;

namespace {

const AccountTag kAlice{1400000000ULL, QStringLiteral("alice")};
const AccountTag kBob{1400000000ULL, QStringLiteral("bob")};
const QString kPeer = QStringLiteral("faulty-client");

AttachmentEvent makeEvent(const AccountTag& account,
                          const QString& peerId,
                          AttachmentPhase phase,
                          const QString& messageId = QStringLiteral("m1"))
{
    AttachmentEvent e;
    e.account = account;
    e.peerId = peerId;
    e.phase = phase;
    e.messageId = messageId;
    e.atMs = 1'000;
    return e;
}

}  // namespace

class RemoteDiagnosticsEvidenceTest : public QObject {
    Q_OBJECT

private slots:
    void exportsOnlyTheAskingAccountsOwnEntries();
    void doesNotLeakASameNamedFriendFromAnotherAccount();
    void reconnectingTheSameAccountIsNotAnAccountSwitch();
    void refusesEntriesWithIncompleteOwnership();
    void keepsEveryPhaseDistinctIncludingTheTwoFailureKinds();
    void ringBufferDropsOldestNotNewest();
    void separatesThisReportsFailureFromAnyOtherAttachment();
};

void RemoteDiagnosticsEvidenceTest::exportsOnlyTheAskingAccountsOwnEntries()
{
    EvidenceLog log;
    log.record(makeEvent(kAlice, kPeer, AttachmentPhase::DownloadStarted));
    log.record(makeEvent(kBob, kPeer, AttachmentPhase::Delivered));

    const QList<AttachmentEvent> alice = log.exportFor(kAlice, kPeer);
    QCOMPARE(alice.size(), 1);
    QCOMPARE(static_cast<int>(alice.at(0).phase),
             static_cast<int>(AttachmentPhase::DownloadStarted));
}

// 这条就是「换账号后与同名好友的旧记录串进来」那个场景。
// 好友 ID 完全相同，只有账号不同——只按 peerId 筛必然出错。
void RemoteDiagnosticsEvidenceTest::doesNotLeakASameNamedFriendFromAnotherAccount()
{
    EvidenceLog log;
    log.record(makeEvent(kAlice, kPeer, AttachmentPhase::Delivered,
                     QStringLiteral("alice-secret-msg")));

    const QList<AttachmentEvent> bob = log.exportFor(kBob, kPeer);
    QVERIFY2(bob.isEmpty(), "another account's entry must never be exported");
}

// 同账号断线重连仍是同一个账号，不能被当成换号把自己的记录挡掉。
void RemoteDiagnosticsEvidenceTest::reconnectingTheSameAccountIsNotAnAccountSwitch()
{
    EvidenceLog log;
    log.record(makeEvent(kAlice, kPeer, AttachmentPhase::DownloadStarted));
    // 重连后重新构造的标签：值相同，对象不同。
    const AccountTag reconnected{kAlice.sdkAppId, QString(kAlice.ownerUserId)};
    QVERIFY(reconnected == kAlice);
    QCOMPARE(log.exportFor(reconnected, kPeer).size(), 1);
}

void RemoteDiagnosticsEvidenceTest::refusesEntriesWithIncompleteOwnership()
{
    EvidenceLog log;
    log.record(makeEvent(AccountTag{}, kPeer, AttachmentPhase::Delivered));                    // 无账号
    log.record(makeEvent(AccountTag{0, QStringLiteral("alice")}, kPeer, AttachmentPhase::Delivered));
    log.record(makeEvent(AccountTag{1, QString()}, kPeer, AttachmentPhase::Delivered));
    log.record(makeEvent(kAlice, QString(), AttachmentPhase::Delivered));                      // 无好友
    QCOMPARE(log.size(), 0);
}

// 下载失败与写入失败是两件事：前者没拿到字节，后者拿到了却没存住。
// 都不能记成 Delivered——报告要能回答「卡在哪一步」。
void RemoteDiagnosticsEvidenceTest::keepsEveryPhaseDistinctIncludingTheTwoFailureKinds()
{
    const QList<AttachmentPhase> phases{
        AttachmentPhase::MetadataReceived, AttachmentPhase::DownloadStarted,
        AttachmentPhase::CacheHit,         AttachmentPhase::DownloadFailed,
        AttachmentPhase::WriteFailed,      AttachmentPhase::Delivered};

    QSet<QString> descriptions;
    for (AttachmentPhase phase : phases) {
        const QString text = describeAttachmentPhase(phase);
        QVERIFY2(!text.isEmpty(), "every phase needs wording");
        descriptions.insert(text);
    }
    QCOMPARE(descriptions.size(), phases.size());

    EvidenceLog log;
    for (AttachmentPhase phase : phases) log.record(makeEvent(kAlice, kPeer, phase));
    QCOMPARE(log.exportFor(kAlice, kPeer).size(), phases.size());
}

// 满了要丢最旧的。丢最新的话，正在排查的那次反而先没了。
void RemoteDiagnosticsEvidenceTest::ringBufferDropsOldestNotNewest()
{
    EvidenceLog log(/*capacity*/ 3);
    for (int i = 0; i < 5; ++i) {
        log.record(makeEvent(kAlice, kPeer, AttachmentPhase::Delivered,
                         QStringLiteral("m%1").arg(i)));
    }
    const QList<AttachmentEvent> kept = log.exportFor(kAlice, kPeer);
    QCOMPARE(kept.size(), 3);
    QCOMPARE(kept.first().messageId, QStringLiteral("m2"));
    QCOMPARE(kept.last().messageId, QStringLiteral("m4"));
}

// 控制器要靠 account+peer+requestId 三者同时匹配才提前收尾。
// 只按 account+peer 的话，同一好友随便一个附件下载失败都会被当成
// 「本次排障报告没下来」，从而提前发一份说错了原因的部分报告。
void RemoteDiagnosticsEvidenceTest::separatesThisReportsFailureFromAnyOtherAttachment()
{
    EvidenceLog log;
    const QString mine = QStringLiteral("65de6748-3a4e-4d08-8ffd-bc78e1804ff9");

    AttachmentEvent unrelated = makeEvent(kAlice, kPeer, AttachmentPhase::DownloadFailed,
                                          QStringLiteral("m-photo"));
    unrelated.requestId.clear();  // 普通附件：没有 requestId
    log.record(unrelated);

    AttachmentEvent ours = makeEvent(kAlice, kPeer, AttachmentPhase::DownloadFailed,
                                     QStringLiteral("m-report"));
    ours.requestId = mine;
    log.record(ours);

    const QList<AttachmentEvent> scene = log.exportFor(kAlice, kPeer);
    // 两条都属于现场元数据，都要留在报告里。
    QCOMPARE(scene.size(), 2);

    // 但只有一条能作为「本次报告下载失败」的依据。
    int failuresForThisRequest = 0;
    for (const AttachmentEvent& e : scene) {
        if (e.requestId == mine && e.phase == AttachmentPhase::DownloadFailed) {
            ++failuresForThisRequest;
        }
    }
    QCOMPARE(failuresForThisRequest, 1);
}

QTEST_MAIN(RemoteDiagnosticsEvidenceTest)
#include "RemoteDiagnosticsEvidenceTest.moc"
