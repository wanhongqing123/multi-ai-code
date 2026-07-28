#include "remote/RemoteDesktopController.h"

#include <QUuid>

#include "remote/RemoteDesktopAuth.h"
#include "remote/RemoteDesktopSession.h"

using namespace RemoteDesktop;
using RemoteDesktopSignals::Signal;
using RemoteDesktopSignals::Type;

namespace {

QString defaultId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
}

}  // namespace

RemoteDesktopController::RemoteDesktopController(Config config,
                                                 RemoteDesktopSettings settings,
                                                 std::unique_ptr<ITrtcEngine> engine,
                                                 SendSignal sendSignal,
                                                 QObject* parent)
    : QObject(parent),
      config_(std::move(config)),
      settings_(std::move(settings)),
      engine_(std::move(engine)),
      sendSignal_(std::move(sendSignal)),
      idGenerator_(defaultId) {}

RemoteDesktopController::~RemoteDesktopController() {
    if (engine_) engine_->stop();
}

void RemoteDesktopController::setIdGenerator(IdGenerator generator) {
    if (generator) idGenerator_ = std::move(generator);
}

HostState RemoteDesktopController::hostState() const { return hostState_; }
ViewerState RemoteDesktopController::viewerState() const { return viewerState_; }
const RemoteDesktopSettings& RemoteDesktopController::settings() const { return settings_; }

void RemoteDesktopController::updateSettings(const RemoteDesktopSettings& settings) {
    settings_ = settings;
}

void RemoteDesktopController::send(const QString& peerId, const Signal& signal) {
    if (!sendSignal_ || peerId.isEmpty()) return;
    const QString text = RemoteDesktopSignals::encodeSignal(signal);
    if (!text.isEmpty()) sendSignal_(peerId, text);
}

TrtcRoomParams RemoteDesktopController::roomParams(const QString& roomId) const {
    TrtcRoomParams params;
    params.sdkAppId = config_.sdkAppId;
    params.userId = config_.localUserId;
    params.roomId = roomId;
    if (config_.userSigProvider) params.userSig = config_.userSigProvider(config_.localUserId);
    return params;
}

bool RemoteDesktopController::handleIncomingText(const QString& fromUserId, const QString& text) {
    if (!RemoteDesktopSignals::isSignalText(text)) return false;

    const Signal signal = RemoteDesktopSignals::decodeSignal(text);
    // 前缀对但解析失败：仍然消费掉，避免半截协议文本泄漏到聊天记录里。
    if (signal.type == Type::Unknown) return true;

    switch (signal.type) {
        case Type::Invite:
            handleInvite(fromUserId, signal);
            return true;
        case Type::Accept:
        case Type::Reject: {
            // 只认当前会话的响应，忽略过期会话的迟到信令。
            if (signal.sessionId != viewerSessionId_) return true;
            const ViewerTransition transition = viewerOnSignal(viewerState_, signal);
            if (transition.nextState == ViewerState::Connecting) {
                viewerRoomId_ = signal.roomId.isEmpty() ? viewerRoomId_ : signal.roomId;
                // 观看端的渲染窗口由 UI 层在收到状态变化后提供，这里先进房。
                engine_->startViewing(roomParams(viewerRoomId_), nullptr);
            }
            setViewerState(transition.nextState, transition.failureReason);
            return true;
        }
        case Type::Stop: {
            if (!viewerSessionId_.isEmpty() && signal.sessionId == viewerSessionId_) {
                engine_->stop();
                setViewerState(viewerOnSignal(viewerState_, signal).nextState);
                viewerSessionId_.clear();
                viewerRoomId_.clear();
                viewerPeerId_.clear();
            }
            if (!hostSessionId_.isEmpty() && signal.sessionId == hostSessionId_) {
                const HostDecision decision = decideOnPeerGone(hostState_);
                applyHostDecision(decision, fromUserId);
            }
            return true;
        }
        case Type::Unknown:
            break;
    }
    return true;
}

void RemoteDesktopController::handleInvite(const QString& fromUserId, const Signal& signal) {
    HostInviteInput input;
    input.mode = settings_.effectiveMode();
    input.currentState = hostState_;
    input.senderAllowed = settings_.isSenderAllowed(fromUserId);
    input.consecutiveFailures = settings_.consecutiveAuthFailures;
    // 密码校验只在无人值守模式下才有意义；proof 绑定本次会话参数。
    input.authProofValid = RemoteDesktopAuth::verifyAuthProof(
        settings_.secret, signal.authProof, signal.sessionId, signal.roomId, fromUserId);

    const HostDecision decision = decideOnInvite(input);
    recordAuthAttempt(input, decision);

    hostSessionId_ = signal.sessionId;
    hostRoomId_ = signal.roomId;
    applyHostDecision(decision, fromUserId);
}

void RemoteDesktopController::recordAuthAttempt(const HostInviteInput& input,
                                                const HostDecision& decision) {
    // 只有真正走到密码校验那一步才计数：有人值守模式没有密码这回事，
    // 非白名单/忙碌被挡下的请求也不该影响爆破计数。
    const bool passwordWasChecked = input.mode == HostMode::Unattended && input.senderAllowed
                                    && input.currentState == HostState::Idle;
    if (!passwordWasChecked) return;

    settings_.consecutiveAuthFailures = input.authProofValid ? 0 : input.consecutiveFailures + 1;
    if (decision.downgradeToAttended) {
        settings_.mode = HostMode::Attended;
        settings_.consecutiveAuthFailures = 0;
        emit modeDowngraded();
    }
    emit settingsChanged(settings_);
}

void RemoteDesktopController::applyHostDecision(const HostDecision& decision,
                                                const QString& peerId) {
    switch (decision.action) {
        case HostAction::ShowConsentDialog:
            hostPeerId_ = peerId;
            hostState_ = decision.nextState;
            emit consentRequested(peerId);
            break;
        case HostAction::AcceptAndShare: {
            hostPeerId_ = peerId;
            const bool started = engine_->startScreenShare(roomParams(hostRoomId_));
            if (!started) {
                // 进房失败要如实回拒，不能让对方停在"连接中"。
                hostState_ = HostState::Idle;
                Signal reject;
                reject.type = Type::Reject;
                reject.sessionId = hostSessionId_;
                reject.reason = QStringLiteral("屏幕共享启动失败");
                send(peerId, reject);
                break;
            }
            hostState_ = decision.nextState;
            Signal accept;
            accept.type = Type::Accept;
            accept.sessionId = hostSessionId_;
            accept.roomId = hostRoomId_;
            send(peerId, accept);
            emit sharingStarted(peerId);
            break;
        }
        case HostAction::RejectInvite: {
            Signal reject;
            reject.type = Type::Reject;
            reject.sessionId = hostSessionId_;
            reject.reason = decision.reason;
            send(peerId, reject);
            hostState_ = decision.nextState;
            // 被拒绝的会话不保留上下文，避免后续 stop 误匹配。
            if (hostState_ == HostState::Idle) {
                hostSessionId_.clear();
                hostRoomId_.clear();
                hostPeerId_.clear();
            }
            break;
        }
        case HostAction::StopSharing: {
            engine_->stop();
            hostState_ = decision.nextState;
            Signal stop;
            stop.type = Type::Stop;
            stop.sessionId = hostSessionId_;
            send(peerId.isEmpty() ? hostPeerId_ : peerId, stop);
            hostSessionId_.clear();
            hostRoomId_.clear();
            hostPeerId_.clear();
            emit sharingStopped();
            break;
        }
        case HostAction::None:
            hostState_ = decision.nextState;
            break;
    }
}

void RemoteDesktopController::requestView(const QString& peerId, const QString& password) {
    if (!canStartInvite(viewerState_)) return;

    viewerPeerId_ = peerId;
    viewerSessionId_ = idGenerator_();
    viewerRoomId_ = QStringLiteral("mc-%1-%2").arg(config_.localUserId, idGenerator_());

    Signal invite;
    invite.type = Type::Invite;
    invite.sessionId = viewerSessionId_;
    invite.roomId = viewerRoomId_;
    if (!password.isEmpty()) {
        // proof 用对端 salt 派生——一期由用户输入密码时本地保存配对信息；
        // 尚未配对时退化为空 proof，由被控端按自身模式决定是否需要。
        const RemoteDesktopAuth::StoredSecret secret =
            RemoteDesktopAuth::deriveSecret(password, settings_.secret.salt);
        invite.authProof = RemoteDesktopAuth::makeAuthProof(
            secret, invite.sessionId, invite.roomId, config_.localUserId);
    }
    send(peerId, invite);
    setViewerState(ViewerState::Inviting);
}

void RemoteDesktopController::resolveConsent(bool accepted) {
    const HostDecision decision = decideOnConsentResult(hostState_, accepted);
    applyHostDecision(decision, hostPeerId_);
}

void RemoteDesktopController::stopSession() {
    if (hostState_ == HostState::Sharing) {
        applyHostDecision(decideOnPeerGone(hostState_), hostPeerId_);
    }
    if (viewerState_ != ViewerState::Idle) {
        engine_->stop();
        Signal stop;
        stop.type = Type::Stop;
        stop.sessionId = viewerSessionId_;
        send(viewerPeerId_, stop);
        setViewerState(viewerOnLocalClose(viewerState_).nextState);
        viewerSessionId_.clear();
        viewerRoomId_.clear();
        viewerPeerId_.clear();
    }
}

void RemoteDesktopController::setViewerState(ViewerState state, const QString& failureReason) {
    if (state == viewerState_ && failureReason.isEmpty()) return;
    viewerState_ = state;
    emit viewerStateChanged(state, failureReason);
}
