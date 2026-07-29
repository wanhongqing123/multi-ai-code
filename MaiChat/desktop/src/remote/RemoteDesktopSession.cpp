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

bool shouldAcceptRemoteInput(HostState currentState, bool allowRemoteControl,
                             const QString& currentSessionId, const QString& currentPeerUserId,
                             const QString& packetSessionId, const QString& fromUserId) {
    // 没在共享就不该有输入进来。
    if (currentState != HostState::Sharing) return false;
    // 控制权限是独立开关，默认关：能看不等于能操作。
    if (!allowRemoteControl) return false;
    // 会话 ID 必须对得上，挡掉上一场会话的残留包。
    if (currentSessionId.isEmpty() || packetSessionId != currentSessionId) return false;
    // 只认当前会话的对端。房间里若混进第三方，它的输入一律不执行。
    if (currentPeerUserId.isEmpty() || fromUserId != currentPeerUserId) return false;
    return true;
}

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

    // 无人值守 + 未设密码：白名单即授权，直接放行。主场景是「自己在外面连
    // 自己的电脑」，每次输密码的摩擦不值得——安全性由白名单与 IM 账号本身承担。
    if (!input.passwordConfigured) {
        decision.action = HostAction::AcceptAndShare;
        decision.nextState = HostState::Sharing;
        return decision;
    }

    // 设了密码就必须校验通过，否则拒绝。
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

bool canStartInvite(ViewerState current) {
    // 只有空闲或上一次失败后才允许发起；进行中的会话不被覆盖。
    return current == ViewerState::Idle || current == ViewerState::Failed;
}

ViewerTransition viewerOnInviteSent(ViewerState current) {
    ViewerTransition transition;
    transition.nextState = canStartInvite(current) ? ViewerState::Inviting : current;
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
