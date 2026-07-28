#include <QtTest/QtTest>

#include "remote/RemoteDesktopAuth.h"
#include "remote/RemoteDesktopSession.h"

using namespace RemoteDesktop;
using RemoteDesktopSignals::Signal;
using RemoteDesktopSignals::Type;

class RemoteDesktopSessionTest : public QObject {
    Q_OBJECT

private slots:
    // 被控端
    void attendedModeAsksForConsent();
    void unattendedModeAutoAcceptsWithValidProof();
    void unattendedModeRejectsInvalidProof();
    void unattendedModeDowngradesAfterRepeatedFailures();
    void disabledModeRejectsEveryInvite();
    void rejectsSenderOutsideAllowList();
    void rejectsSecondInviteWhileSharing();
    void whitelistCheckPrecedesModeCheck();
    void consentAcceptStartsSharing();
    void consentDeclineReturnsToIdle();
    void ignoresConsentResultWhenNotAwaiting();
    void peerGoneStopsSharing();

    // 控制端
    void viewerFollowsHappyPath();
    void viewerFailsOnReject();
    void viewerFailsOnTimeout();
    void viewerIgnoresLateAccept();
    void viewerIgnoresTimeoutWhileViewing();
    void viewerStopsOnPeerStop();
    void viewerDoesNotReinviteWhileActive();
};

namespace {

HostInviteInput inviteInput(HostMode mode, bool allowed = true, bool proofValid = false) {
    HostInviteInput input;
    input.mode = mode;
    input.senderAllowed = allowed;
    input.authProofValid = proofValid;
    return input;
}

Signal signalOf(Type type, const QString& reason = QString()) {
    Signal signal;
    signal.type = type;
    signal.reason = reason;
    return signal;
}

}  // namespace

void RemoteDesktopSessionTest::attendedModeAsksForConsent() {
    const HostDecision decision = decideOnInvite(inviteInput(HostMode::Attended));
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::ShowConsentDialog));
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::AwaitingConsent));
    QVERIFY(!decision.downgradeToAttended);
}

void RemoteDesktopSessionTest::unattendedModeAutoAcceptsWithValidProof() {
    const HostDecision decision =
        decideOnInvite(inviteInput(HostMode::Unattended, /*allowed=*/true, /*proofValid=*/true));
    // 无人值守的核心承诺：密码对了就不打扰用户，直接进入共享。
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::AcceptAndShare));
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::Sharing));
}

void RemoteDesktopSessionTest::unattendedModeRejectsInvalidProof() {
    const HostDecision decision =
        decideOnInvite(inviteInput(HostMode::Unattended, /*allowed=*/true, /*proofValid=*/false));
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::RejectInvite));
    QCOMPARE(decision.reason, reasonBadPassword());
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::Idle));
}

void RemoteDesktopSessionTest::unattendedModeDowngradesAfterRepeatedFailures() {
    HostInviteInput input = inviteInput(HostMode::Unattended, true, false);

    input.consecutiveFailures = RemoteDesktopAuth::kMaxConsecutiveFailures - 2;
    QVERIFY(!decideOnInvite(input).downgradeToAttended);

    // 本次失败使连续失败数达到阈值 → 自动降级为有人值守，阻断在线爆破。
    input.consecutiveFailures = RemoteDesktopAuth::kMaxConsecutiveFailures - 1;
    QVERIFY(decideOnInvite(input).downgradeToAttended);
}

void RemoteDesktopSessionTest::disabledModeRejectsEveryInvite() {
    // 即使密码正确，模式为关闭时也不放行。
    const HostDecision decision =
        decideOnInvite(inviteInput(HostMode::Disabled, /*allowed=*/true, /*proofValid=*/true));
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::RejectInvite));
    QCOMPARE(decision.reason, reasonNotEnabled());
}

void RemoteDesktopSessionTest::rejectsSenderOutsideAllowList() {
    const HostDecision decision =
        decideOnInvite(inviteInput(HostMode::Unattended, /*allowed=*/false, /*proofValid=*/true));
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::RejectInvite));
    QCOMPARE(decision.reason, reasonNotAllowed());
}

void RemoteDesktopSessionTest::rejectsSecondInviteWhileSharing() {
    HostInviteInput input = inviteInput(HostMode::Unattended, true, true);
    input.currentState = HostState::Sharing;

    const HostDecision decision = decideOnInvite(input);
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::RejectInvite));
    QCOMPARE(decision.reason, reasonBusy());
    // 已有会话不被抢占。
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::Sharing));
}

void RemoteDesktopSessionTest::whitelistCheckPrecedesModeCheck() {
    // 非白名单账号即使在"关闭"模式下也应得到统一的"不在允许列表"回复，
    // 不泄漏对方到底有没有开启远程桌面。
    const HostDecision decision = decideOnInvite(inviteInput(HostMode::Disabled, false, false));
    QCOMPARE(decision.reason, reasonNotAllowed());
}

void RemoteDesktopSessionTest::consentAcceptStartsSharing() {
    const HostDecision decision = decideOnConsentResult(HostState::AwaitingConsent, true);
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::AcceptAndShare));
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::Sharing));
}

void RemoteDesktopSessionTest::consentDeclineReturnsToIdle() {
    const HostDecision decision = decideOnConsentResult(HostState::AwaitingConsent, false);
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::RejectInvite));
    QCOMPARE(decision.reason, reasonDeclined());
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::Idle));
}

void RemoteDesktopSessionTest::ignoresConsentResultWhenNotAwaiting() {
    // 乱序事件（弹窗已被超时关掉后用户又点了同意）不得把状态机拉进共享。
    const HostDecision decision = decideOnConsentResult(HostState::Idle, true);
    QCOMPARE(static_cast<int>(decision.action), static_cast<int>(HostAction::None));
    QCOMPARE(static_cast<int>(decision.nextState), static_cast<int>(HostState::Idle));
}

void RemoteDesktopSessionTest::peerGoneStopsSharing() {
    const HostDecision sharing = decideOnPeerGone(HostState::Sharing);
    QCOMPARE(static_cast<int>(sharing.action), static_cast<int>(HostAction::StopSharing));
    QCOMPARE(static_cast<int>(sharing.nextState), static_cast<int>(HostState::Idle));

    const HostDecision idle = decideOnPeerGone(HostState::Idle);
    QCOMPARE(static_cast<int>(idle.action), static_cast<int>(HostAction::None));
}

void RemoteDesktopSessionTest::viewerFollowsHappyPath() {
    ViewerState state = viewerOnInviteSent(ViewerState::Idle).nextState;
    QCOMPARE(static_cast<int>(state), static_cast<int>(ViewerState::Inviting));

    state = viewerOnSignal(state, signalOf(Type::Accept)).nextState;
    QCOMPARE(static_cast<int>(state), static_cast<int>(ViewerState::Connecting));

    state = viewerOnFirstFrame(state).nextState;
    QCOMPARE(static_cast<int>(state), static_cast<int>(ViewerState::Viewing));
}

void RemoteDesktopSessionTest::viewerFailsOnReject() {
    const ViewerTransition transition =
        viewerOnSignal(ViewerState::Inviting, signalOf(Type::Reject, reasonBusy()));
    QCOMPARE(static_cast<int>(transition.nextState), static_cast<int>(ViewerState::Failed));
    QCOMPARE(transition.failureReason, reasonBusy());

    // 对端没给原因时也要有可读兜底文案。
    const ViewerTransition bare = viewerOnSignal(ViewerState::Inviting, signalOf(Type::Reject));
    QCOMPARE(bare.failureReason, reasonDeclined());
}

void RemoteDesktopSessionTest::viewerFailsOnTimeout() {
    for (ViewerState state : {ViewerState::Inviting, ViewerState::Connecting}) {
        const ViewerTransition transition = viewerOnTimeout(state);
        QCOMPARE(static_cast<int>(transition.nextState), static_cast<int>(ViewerState::Failed));
        QCOMPARE(transition.failureReason, reasonTimeout());
    }
}

void RemoteDesktopSessionTest::viewerIgnoresLateAccept() {
    // 会话已结束后迟到的 accept 不得把观看窗重新拉起来。
    const ViewerTransition transition = viewerOnSignal(ViewerState::Idle, signalOf(Type::Accept));
    QCOMPARE(static_cast<int>(transition.nextState), static_cast<int>(ViewerState::Idle));
}

void RemoteDesktopSessionTest::viewerIgnoresTimeoutWhileViewing() {
    const ViewerTransition transition = viewerOnTimeout(ViewerState::Viewing);
    QCOMPARE(static_cast<int>(transition.nextState), static_cast<int>(ViewerState::Viewing));
    QVERIFY(transition.failureReason.isEmpty());
}

void RemoteDesktopSessionTest::viewerStopsOnPeerStop() {
    for (ViewerState state : {ViewerState::Inviting, ViewerState::Connecting, ViewerState::Viewing}) {
        QCOMPARE(static_cast<int>(viewerOnSignal(state, signalOf(Type::Stop)).nextState),
                 static_cast<int>(ViewerState::Idle));
    }
}

void RemoteDesktopSessionTest::viewerDoesNotReinviteWhileActive() {
    for (ViewerState state : {ViewerState::Inviting, ViewerState::Connecting, ViewerState::Viewing}) {
        QCOMPARE(static_cast<int>(viewerOnInviteSent(state).nextState), static_cast<int>(state));
        // canStartInvite 必须与 viewerOnInviteSent 的"原样返回"语义一致：
        // Inviting 状态下二者都表示"不能再发"，调用方不能只看返回状态。
        QVERIFY(!canStartInvite(state));
    }
    // 失败后允许重试。
    QCOMPARE(static_cast<int>(viewerOnInviteSent(ViewerState::Failed).nextState),
             static_cast<int>(ViewerState::Inviting));
    QVERIFY(canStartInvite(ViewerState::Failed));
    QVERIFY(canStartInvite(ViewerState::Idle));
}

QTEST_MAIN(RemoteDesktopSessionTest)
#include "RemoteDesktopSessionTest.moc"
