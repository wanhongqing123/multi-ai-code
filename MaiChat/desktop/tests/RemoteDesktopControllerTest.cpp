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
// 记录型注入 sink：断言"到底动没动鼠标"，而不是只看有没有报错。
class RecordingInputSink final : public RemoteInput::IRemoteInputSink {
public:
    QStringList calls;

    void moveTo(double x, double y) override {
        calls << QStringLiteral("move %1,%2").arg(x).arg(y);
    }
    void mouseButton(RemoteInput::MouseButton button, bool pressed, double, double) override {
        calls << QStringLiteral("btn %1 %2")
                     .arg(static_cast<int>(button))
                     .arg(pressed ? QStringLiteral("down") : QStringLiteral("up"));
    }
    void wheel(int delta, double, double) override {
        calls << QStringLiteral("wheel %1").arg(delta);
    }
    void key(quint32 keyCode, bool pressed) override {
        calls << QStringLiteral("key %1 %2")
                     .arg(keyCode)
                     .arg(pressed ? QStringLiteral("down") : QStringLiteral("up"));
    }
    void text(const QString& value) override { calls << QStringLiteral("text %1").arg(value); }
};

class FakeTrtcEngine final : public ITrtcEngine {
public:
    struct SentMessage {
        int cmdId = 0;
        QByteArray payload;
        bool reliable = false;
        bool ordered = false;
    };
    QVector<SentMessage> sentMessages;
    CustomMessageCallback customMessageCallback;

    void setRemoteVideoCallback(RemoteVideoCallback callback) override {
        remoteVideoCallback = std::move(callback);
    }
    void setErrorCallback(ErrorCallback callback) override {
        errorCallback = std::move(callback);
    }
    void setCustomMessageCallback(CustomMessageCallback callback) override {
        customMessageCallback = std::move(callback);
    }
    bool sendCustomMessage(int cmdId, const QByteArray& payload, bool reliable,
                           bool ordered) override {
        if (!active) return false;
        sentMessages.append({cmdId, payload, reliable, ordered});
        return true;
    }
    void bindRemoteView(const QString& userId, void*) override {
        bindCalls += 1;
        lastBoundUserId = userId;
    }

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
    int bindCalls = 0;
    bool active = false;
    bool failNextStart = false;
    QString lastRoomId;
    QString lastBoundUserId;
    RemoteVideoCallback remoteVideoCallback;
    ErrorCallback errorCallback;
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

        // 换掉真实注入器，否则跑一遍测试就真去动鼠标了。
        auto sinkOwner = std::make_unique<RecordingInputSink>();
        sink = sinkOwner.get();
        controller->setInputSink(std::move(sinkOwner));
    }

    // 把一包输入按被控端收到的样子喂进去。
    void deliverInput(const QString& sessionId, quint32 sequence,
                      const QVector<RemoteInput::Event>& events,
                      const QString& fromUserId = kPeerUser) {
        RemoteInput::Packet packet;
        packet.sessionId = sessionId;
        packet.sequence = sequence;
        packet.events = events;
        if (engine->customMessageCallback) {
            engine->customMessageCallback(fromUserId, RemoteInput::kCmdIdReliable,
                                          RemoteInput::encodePacket(packet));
        }
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
    RecordingInputSink* sink = nullptr;
    std::unique_ptr<RemoteDesktopController> controller;
    QVector<SentSignal> sent;
};

}  // namespace

class RemoteDesktopControllerTest : public QObject {
    Q_OBJECT

private slots:
    void firstFrameEnablesInputSending();
    void dropsRemoteInputWhenControlSwitchIsOff();
    void injectsRemoteInputOnceControlIsAllowed();
    void dropsRemoteInputFromNonPeerAndStaleSession();
    void releasesHeldKeysWhenSessionStops();
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
    void doesNotResendInviteWhileOneIsPending();
    void forwardsRemoteVideoHandlerToEngine();
};

namespace {

RemoteInput::Event keyDown(quint32 keyCode) {
    RemoteInput::Event event;
    event.type = RemoteInput::EventType::Key;
    event.keyCode = keyCode;
    event.pressed = true;
    return event;
}

// 让被控端进入共享态。Harness 默认设了密码，所以 invite 必须带上 proof。
void startSharing(Harness& h) {
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s-1"), QStringLiteral("room-1"), kPassword));
    QCOMPARE(h.controller->hostState(), HostState::Sharing);
}

}  // namespace

void RemoteDesktopControllerTest::firstFrameEnablesInputSending() {
    Harness h(HostMode::Attended);
    // UI 会挂自己的画面回调；控制器必须先截一道再转交，不能被覆盖掉。
    int forwarded = 0;
    h.controller->setRemoteVideoHandler(
        [&forwarded](const QString&, bool) { ++forwarded; });

    h.controller->requestView(kPeerUser);
    Signal accept;
    accept.type = Type::Accept;
    accept.sessionId = h.lastSent().sessionId;
    accept.roomId = QStringLiteral("room-1");
    h.controller->handleIncomingText(kPeerUser, RemoteDesktopSignals::encodeSignal(accept));
    QCOMPARE(h.controller->viewerState(), ViewerState::Connecting);

    // 首帧到达是 Connecting → Viewing 的唯一触发点，而输入会话正是跟着
    // Viewing 开的。少了这一步，画面照常显示、控制却一个包都发不出去。
    QVERIFY(h.engine->remoteVideoCallback);
    h.engine->remoteVideoCallback(kPeerUser, true);
    QCOMPARE(h.controller->viewerState(), ViewerState::Viewing);
    QCOMPARE(forwarded, 1);

    h.engine->sentMessages.clear();
    h.controller->inputSender().queueKey(0x41, true);
    h.controller->flushPendingInput(1000);
    QVERIFY2(!h.engine->sentMessages.isEmpty(), "首帧之后仍然发不出输入包");
}

void RemoteDesktopControllerTest::dropsRemoteInputWhenControlSwitchIsOff() {
    Harness h(HostMode::Unattended);
    QVERIFY(!h.controller->isRemoteControlAllowed());  // 默认关
    startSharing(h);

    // 能看不等于能操作：开关没开时，输入必须一条都注不进去。
    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)});
    QVERIFY2(h.sink->calls.isEmpty(),
             qPrintable(QStringLiteral("控制开关关着却注入了：%1")
                            .arg(h.sink->calls.join(QStringLiteral(", ")))));
}

void RemoteDesktopControllerTest::injectsRemoteInputOnceControlIsAllowed() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)});
    QCOMPARE(h.sink->calls, QStringList({QStringLiteral("key 65 down")}));
}

void RemoteDesktopControllerTest::dropsRemoteInputFromNonPeerAndStaleSession() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    // 房间里混进第三方：它的输入一律不执行。
    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)}, QStringLiteral("someone-else"));
    QVERIFY(h.sink->calls.isEmpty());

    // 上一场会话的残留包不该操作这一场的电脑。
    h.deliverInput(QStringLiteral("s-0"), 2, {keyDown(0x42)});
    QVERIFY(h.sink->calls.isEmpty());

    // 对得上的照常放行，确认上面两条不是因为整条链路根本没通。
    h.deliverInput(QStringLiteral("s-1"), 3, {keyDown(0x43)});
    QCOMPARE(h.sink->calls, QStringList({QStringLiteral("key 67 down")}));
}

void RemoteDesktopControllerTest::releasesHeldKeysWhenSessionStops() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x11)});  // Ctrl 按下
    QCOMPARE(h.sink->calls, QStringList({QStringLiteral("key 17 down")}));
    h.sink->calls.clear();

    // 会话结束必须把按住的键抬了：否则被控机一直是 Ctrl 按住状态，
    // 点什么都变成 Ctrl+点击，而人不在电脑旁没法自己解。
    h.controller->stopSession();
    QCOMPARE(h.controller->hostState(), HostState::Idle);
    QVERIFY2(h.sink->calls.contains(QStringLiteral("key 17 up")),
             qPrintable(QStringLiteral("会话结束没抬键，实际调用：%1")
                            .arg(h.sink->calls.join(QStringLiteral(", ")))));
}

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

void RemoteDesktopControllerTest::doesNotResendInviteWhileOneIsPending() {
    Harness h(HostMode::Attended);

    h.controller->requestView(kPeerUser);
    QCOMPARE(h.sent.size(), 1);

    // 用户连点两下 🖥 不应该发两次邀请，也不能把已有会话的 sessionId 冲掉。
    const QString firstSessionId = h.lastSent().sessionId;
    h.controller->requestView(kPeerUser);
    QCOMPARE(h.sent.size(), 1);
    QCOMPARE(h.lastSent().sessionId, firstSessionId);
}

void RemoteDesktopControllerTest::forwardsRemoteVideoHandlerToEngine() {
    Harness h(HostMode::Attended);

    QString seenUser;
    bool seenAvailable = false;
    h.controller->setRemoteVideoHandler([&](const QString& userId, bool available) {
        seenUser = userId;
        seenAvailable = available;
    });
    QVERIFY(h.engine->remoteVideoCallback != nullptr);

    // 模拟 SDK 通知远端画面到达 → UI 层据此才去绑渲染窗口。
    h.engine->remoteVideoCallback(kPeerUser, true);
    QCOMPARE(seenUser, kPeerUser);
    QVERIFY(seenAvailable);

    h.controller->bindRemoteView(kPeerUser, nullptr);
    QCOMPARE(h.engine->bindCalls, 1);
    QCOMPARE(h.engine->lastBoundUserId, kPeerUser);
}

QTEST_MAIN(RemoteDesktopControllerTest)
#include "RemoteDesktopControllerTest.moc"
