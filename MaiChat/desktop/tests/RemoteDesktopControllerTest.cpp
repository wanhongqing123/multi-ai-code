#include <QtTest/QtTest>

#include <memory>

#include "remote/RemoteDesktopAuth.h"
#include "remote/RemoteDesktopController.h"
#include "remote/RemoteDesktopSignal.h"

using namespace RemoteDesktop;
using RemoteDesktopSignals::Signal;
using RemoteDesktopSignals::Type;

namespace {

// 记录调用的假引擎：让"是否真的进房推流了"成为可断言的事实，
// 而不是靠读代码推断。
class FakeTrtcEngine final : public ITrtcEngine {
public:
    QString sdkVersion() const override { return QStringLiteral("0.0.0-fake"); }

    bool startScreenShare(const TrtcRoomParams& params) override {
        if (failNextStart) return false;
        shareCalls += 1;
        lastRoomId = params.roomId;
        active = true;
        return true;
    }

    bool startViewing(const TrtcRoomParams& params, void*) override {
        viewCalls += 1;
        lastRoomId = params.roomId;
        active = true;
        return true;
    }

    void stop() override {
        stopCalls += 1;
        active = false;
    }

    bool isActive() const override { return active; }

    int shareCalls = 0;
    int viewCalls = 0;
    int stopCalls = 0;
    bool active = false;
    bool failNextStart = false;
    QString lastRoomId;
};

struct SentSignal {
    QString peerId;
    Signal signal;
};

const QString kHostUser = QStringLiteral("desktop-im");
const QString kPeerUser = QStringLiteral("whq-iphone");
const QString kPassword = QStringLiteral("pw-remote-1");

class Harness {
public:
    explicit Harness(HostMode mode, bool withPassword = true, bool allowPeer = true) {
        settings.mode = mode;
        if (withPassword) {
            settings.secret =
                RemoteDesktopAuth::deriveSecret(kPassword, RemoteDesktopAuth::generateSalt());
        }
        if (allowPeer) settings.allowedUserIds = QStringList{kPeerUser};

        RemoteDesktopController::Config config;
        config.sdkAppId = 1600148979;
        config.localUserId = kHostUser;
        config.userSigProvider = [](const QString&) { return QStringLiteral("sig"); };

        auto engineOwner = std::make_unique<FakeTrtcEngine>();
        engine = engineOwner.get();

        controller = std::make_unique<RemoteDesktopController>(
            config, settings, std::move(engineOwner),
            [this](const QString& peerId, const QString& text) {
                sent.append({peerId, RemoteDesktopSignals::decodeSignal(text)});
            });
        controller->setIdGenerator([] { return QStringLiteral("fixed-id"); });
    }

    // 构造一条来自 kPeerUser 的 invite，proof 按需带上。
    QString inviteText(const QString& sessionId,
                       const QString& roomId,
                       const QString& password = QString()) const {
        Signal invite;
        invite.type = Type::Invite;
        invite.sessionId = sessionId;
        invite.roomId = roomId;
        if (!password.isEmpty()) {
            const RemoteDesktopAuth::StoredSecret peerSecret =
                RemoteDesktopAuth::deriveSecret(password, settings.secret.salt);
            invite.authProof =
                RemoteDesktopAuth::makeAuthProof(peerSecret, sessionId, roomId, kPeerUser);
        }
        return RemoteDesktopSignals::encodeSignal(invite);
    }

    Signal lastSent() const { return sent.isEmpty() ? Signal{} : sent.last().signal; }

    RemoteDesktopSettings settings;
    FakeTrtcEngine* engine = nullptr;
    std::unique_ptr<RemoteDesktopController> controller;
    QVector<SentSignal> sent;
};

}  // namespace

class RemoteDesktopControllerTest : public QObject {
    Q_OBJECT

private slots:
    void ignoresPlainChatText();
    void consumesMalformedSignalWithoutActing();
    void attendedModeEmitsConsentAndSharesAfterAccept();
    void attendedModeRejectsWhenUserDeclines();
    void unattendedModeSharesWithoutPrompting();
    void unattendedModeRejectsWrongPassword();
    void unattendedModeDowngradesAfterRepeatedFailures();
    void rejectsSenderOutsideAllowList();
    void rejectsInviteWhileAlreadySharing();
    void reportsRejectionToViewer();
    void stopsSharingOnPeerStop();
    void reportsFailureWhenScreenShareCannotStart();
};

void RemoteDesktopControllerTest::ignoresPlainChatText() {
    Harness h(HostMode::Attended);
    QVERIFY(!h.controller->handleIncomingText(kPeerUser, QStringLiteral("你好")));
    QVERIFY(h.sent.isEmpty());
}

void RemoteDesktopControllerTest::consumesMalformedSignalWithoutActing() {
    Harness h(HostMode::Attended);
    // 前缀对但内容坏：必须消费掉（不泄漏到聊天记录），但不产生任何动作。
    const QString broken = RemoteDesktopSignals::signalPrefix() + QStringLiteral("{oops");
    QVERIFY(h.controller->handleIncomingText(kPeerUser, broken));
    QVERIFY(h.sent.isEmpty());
    QCOMPARE(h.engine->shareCalls, 0);
}

void RemoteDesktopControllerTest::attendedModeEmitsConsentAndSharesAfterAccept() {
    Harness h(HostMode::Attended);
    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
    QSignalSpy startedSpy(h.controller.get(), &RemoteDesktopController::sharingStarted);

    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"))));

    QCOMPARE(consentSpy.count(), 1);
    // 用户还没点，绝不能先推流。
    QCOMPARE(h.engine->shareCalls, 0);
    QVERIFY(h.sent.isEmpty());

    h.controller->resolveConsent(true);
    QCOMPARE(h.engine->shareCalls, 1);
    QCOMPARE(h.engine->lastRoomId, QStringLiteral("room-1"));
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Accept));
}

void RemoteDesktopControllerTest::attendedModeRejectsWhenUserDeclines() {
    Harness h(HostMode::Attended);
    h.controller->handleIncomingText(kPeerUser,
                                     h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1")));
    h.controller->resolveConsent(false);

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(h.lastSent().reason, reasonDeclined());
    QCOMPARE(h.engine->shareCalls, 0);
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
}

void RemoteDesktopControllerTest::unattendedModeSharesWithoutPrompting() {
    Harness h(HostMode::Unattended);
    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);

    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword)));

    // 无人值守的核心承诺：不打扰用户，直接开始共享。
    QCOMPARE(consentSpy.count(), 0);
    QCOMPARE(h.engine->shareCalls, 1);
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Accept));
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Sharing));
}

void RemoteDesktopControllerTest::unattendedModeRejectsWrongPassword() {
    Harness h(HostMode::Unattended);
    QVERIFY(h.controller->handleIncomingText(
        kPeerUser,
        h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), QStringLiteral("wrong"))));

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(h.lastSent().reason, reasonBadPassword());
    QCOMPARE(h.engine->shareCalls, 0);
    QCOMPARE(h.controller->settings().consecutiveAuthFailures, 1);
}

void RemoteDesktopControllerTest::unattendedModeDowngradesAfterRepeatedFailures() {
    Harness h(HostMode::Unattended);
    QSignalSpy downgradeSpy(h.controller.get(), &RemoteDesktopController::modeDowngraded);

    for (int i = 0; i < RemoteDesktopAuth::kMaxConsecutiveFailures; ++i) {
        h.controller->handleIncomingText(
            kPeerUser,
            h.inviteText(QStringLiteral("s%1").arg(i), QStringLiteral("room-1"),
                         QStringLiteral("wrong")));
    }

    QCOMPARE(downgradeSpy.count(), 1);
    // 降级后同一个正确密码也不再自动放行，而是走弹窗。
    QCOMPARE(static_cast<int>(h.controller->settings().mode), static_cast<int>(HostMode::Attended));

    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s-final"), QStringLiteral("room-2"), kPassword));
    QCOMPARE(consentSpy.count(), 1);
    QCOMPARE(h.engine->shareCalls, 0);
}

void RemoteDesktopControllerTest::rejectsSenderOutsideAllowList() {
    Harness h(HostMode::Unattended, /*withPassword=*/true, /*allowPeer=*/false);
    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword)));

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    // 即使密码正确，不在白名单也一律拒绝，且不泄漏本机模式。
    QCOMPARE(h.lastSent().reason, reasonNotAllowed());
    QCOMPARE(h.engine->shareCalls, 0);
}

void RemoteDesktopControllerTest::rejectsInviteWhileAlreadySharing() {
    Harness h(HostMode::Unattended);
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword));
    QCOMPARE(h.engine->shareCalls, 1);

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s2"), QStringLiteral("room-2"), kPassword));

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(h.lastSent().reason, reasonBusy());
    // 已有会话不被抢占。
    QCOMPARE(h.engine->shareCalls, 1);
    QCOMPARE(h.engine->lastRoomId, QStringLiteral("room-1"));
}

void RemoteDesktopControllerTest::reportsRejectionToViewer() {
    Harness h(HostMode::Attended);
    QSignalSpy stateSpy(h.controller.get(), &RemoteDesktopController::viewerStateChanged);

    h.controller->requestView(kPeerUser);
    QCOMPARE(static_cast<int>(h.controller->viewerState()), static_cast<int>(ViewerState::Inviting));
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Invite));
    const QString sessionId = h.lastSent().sessionId;

    Signal reject;
    reject.type = Type::Reject;
    reject.sessionId = sessionId;
    reject.reason = reasonBusy();
    h.controller->handleIncomingText(kPeerUser, RemoteDesktopSignals::encodeSignal(reject));

    QCOMPARE(static_cast<int>(h.controller->viewerState()), static_cast<int>(ViewerState::Failed));
    QCOMPARE(stateSpy.last().at(1).toString(), reasonBusy());
}

void RemoteDesktopControllerTest::stopsSharingOnPeerStop() {
    Harness h(HostMode::Unattended);
    QSignalSpy stoppedSpy(h.controller.get(), &RemoteDesktopController::sharingStopped);

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword));

    Signal stop;
    stop.type = Type::Stop;
    stop.sessionId = QStringLiteral("s1");
    h.controller->handleIncomingText(kPeerUser, RemoteDesktopSignals::encodeSignal(stop));

    QCOMPARE(h.engine->stopCalls, 1);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
}

void RemoteDesktopControllerTest::reportsFailureWhenScreenShareCannotStart() {
    Harness h(HostMode::Unattended);
    h.engine->failNextStart = true;

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword));

    // 进房失败必须如实回拒，否则对端会一直卡在"连接中"。
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
}

QTEST_MAIN(RemoteDesktopControllerTest)
#include "RemoteDesktopControllerTest.moc"
