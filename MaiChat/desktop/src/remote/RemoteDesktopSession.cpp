#include "remote/RemoteDesktopSession.h"

#include "remote/RemoteDesktopAuth.h"

namespace RemoteDesktop {

using RemoteDesktopSignals::Signal;
using RemoteDesktopSignals::Type;

QString reasonNotEnabled()  { return QStringLiteral("对方未开启远程桌面"); }
QString reasonNotAllowed()  { return QStringLiteral("你不在对方的允许列表中"); }
QString reasonBusy()        { return QStringLiteral("对方正在共享中"); }
QString reasonBadPassword() { return QStringLiteral("访问密码错误"); }
QString reasonDeclined()    { return QStringLiteral("对方拒绝了请求"); }
QString reasonTimeout()     { return QStringLiteral("对方未在规定时间内响应"); }

HostDecision decideOnInvite(const HostInviteInput& input) {
    HostDecision decision;
    decision.nextState = input.currentState;

    // 已在共享中：不抢占已有会话，直接拒绝并告知原因。
    if (input.currentState != HostState::Idle) {
        decision.action = HostAction::RejectInvite;
        decision.reason = reasonBusy();
        return decision;
    }

    // 白名单先于模式判断：不在允许列表里的账号，连"对方开没开远程"都不该知道。
    if (!input.senderAllowed) {
        decision.action = HostAction::RejectInvite;
        decision.reason = reasonNotAllowed();
        return decision;
    }

    if (input.mode == HostMode::Disabled) {
        decision.action = HostAction::RejectInvite;
        decision.reason = reasonNotEnabled();
        return decision;
    }

    if (input.mode == HostMode::Attended) {
        decision.action = HostAction::ShowConsentDialog;
        decision.nextState = HostState::AwaitingConsent;
        return decision;
    }

    // 无人值守：密码是唯一的自动放行凭据，校验不过一律拒绝。
    if (!input.authProofValid) {
        decision.action = HostAction::RejectInvite;
        decision.reason = reasonBadPassword();
        // 连续失败到阈值就降级回有人值守，避免在线爆破。
        decision.downgradeToAttended =
            RemoteDesktopAuth::shouldDowngradeToAttended(input.consecutiveFailures + 1);
        return decision;
    }

    decision.action = HostAction::AcceptAndShare;
    decision.nextState = HostState::Sharing;
    return decision;
}

HostDecision decideOnConsentResult(HostState currentState, bool accepted) {
    HostDecision decision;
    decision.nextState = currentState;

    // 只有正在等待确认时这个事件才有意义；乱序到达时保持原状态不动。
    if (currentState != HostState::AwaitingConsent) {
        decision.action = HostAction::None;
        return decision;
    }

    if (!accepted) {
        decision.action = HostAction::RejectInvite;
        decision.reason = reasonDeclined();
        decision.nextState = HostState::Idle;
        return decision;
    }

    decision.action = HostAction::AcceptAndShare;
    decision.nextState = HostState::Sharing;
    return decision;
}

HostDecision decideOnPeerGone(HostState currentState) {
    HostDecision decision;
    if (currentState == HostState::Sharing) {
        decision.action = HostAction::StopSharing;
    } else {
        decision.action = HostAction::None;
    }
    decision.nextState = HostState::Idle;
    return decision;
}

ViewerTransition viewerOnInviteSent(ViewerState current) {
    ViewerTransition transition;
    // 已有进行中的会话时不重复发起。
    transition.nextState = current == ViewerState::Idle || current == ViewerState::Failed
                               ? ViewerState::Inviting
                               : current;
    return transition;
}

ViewerTransition viewerOnSignal(ViewerState current, const Signal& signal) {
    ViewerTransition transition;
    transition.nextState = current;

    switch (signal.type) {
        case Type::Accept:
            // 只有正在等待响应时 accept 才有效：防止过期 accept 把已结束的
            // 会话重新拉起。
            if (current == ViewerState::Inviting) transition.nextState = ViewerState::Connecting;
            break;
        case Type::Reject:
            if (current == ViewerState::Inviting) {
                transition.nextState = ViewerState::Failed;
                transition.failureReason =
                    signal.reason.isEmpty() ? reasonDeclined() : signal.reason;
            }
            break;
        case Type::Stop:
            // 对端随时可以叫停；未开始的会话收到 stop 也直接回 Idle。
            if (current != ViewerState::Idle) transition.nextState = ViewerState::Idle;
            break;
        case Type::Invite:
        case Type::Unknown:
            break;
    }
    return transition;
}

ViewerTransition viewerOnFirstFrame(ViewerState current) {
    ViewerTransition transition;
    transition.nextState = current == ViewerState::Connecting ? ViewerState::Viewing : current;
    return transition;
}

ViewerTransition viewerOnTimeout(ViewerState current) {
    ViewerTransition transition;
    transition.nextState = current;
    // 只有还在等待阶段才算超时；已经在看画面了就忽略。
    if (current == ViewerState::Inviting || current == ViewerState::Connecting) {
        transition.nextState = ViewerState::Failed;
        transition.failureReason = reasonTimeout();
    }
    return transition;
}

ViewerTransition viewerOnLocalClose(ViewerState current) {
    Q_UNUSED(current);
    ViewerTransition transition;
    transition.nextState = ViewerState::Idle;
    return transition;
}

}  // namespace RemoteDesktop
