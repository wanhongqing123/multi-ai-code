#include "remote/RemoteDesktopController.h"

#include <QDateTime>
#include <QTimer>

#include <QUuid>

#include "remote/RemoteDesktopAuth.h"
#include "remote/RemoteDesktopSession.h"
#include "remote/RemoteInputTrace.h"

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
      idGenerator_(defaultId) {
    if (engine_) {
        engine_->setCustomMessageCallback(
            [this](const QString& userId, int cmdId, const QByteArray& payload) {
                handleCustomMessage(userId, cmdId, payload);
            });
    }
    // 被控端默认装上真实的注入器与探针；测试用 setInputSink / setSecureDesktopProbe
    // 换成 Fake，免得跑一遍测试就真去动鼠标。
    setInputSink(RemoteInput::createInputSink());
    setSecureDesktopProbe(RemoteDesktop::createSecureDesktopProbe());

    // 两个定时器都跑在主线程：SDK 的限流计数器无同步，发送必须固定单线程。
    inputFlushTimer_ = new QTimer(this);
    inputFlushTimer_->setInterval(RemoteInput::RemoteInputSender::kMoveIntervalMs);
    connect(inputFlushTimer_, &QTimer::timeout, this,
            [this] { flushPendingInput(QDateTime::currentMSecsSinceEpoch()); });
    inputFlushTimer_->start();

    hostWatchdogTimer_ = new QTimer(this);
    // 500ms 足够：看门狗超时是 5 秒，安全桌面判定有 600ms 防抖。
    hostWatchdogTimer_->setInterval(500);
    connect(hostWatchdogTimer_, &QTimer::timeout, this,
            [this] { tickHostWatchdogs(QDateTime::currentMSecsSinceEpoch()); });
    hostWatchdogTimer_->start();
}

RemoteDesktopController::~RemoteDesktopController() {
    // 析构也要走这条：进程退出前把按住的键抬干净，别把被控机留在 Ctrl 按住的
    // 状态上。sleepBlocker_ 的析构会自己恢复休眠策略。
    stopHostControlSide();
    if (inputSender_.isSessionActive()) {
        inputSender_.queueReleaseAll();
        flushPendingInput(QDateTime::currentMSecsSinceEpoch());
        inputSender_.endSession();
    }
    if (engine_) engine_->stop();
}

void RemoteDesktopController::setInputSink(
    std::unique_ptr<RemoteInput::IRemoteInputSink> sink) {
    injector_ = std::make_unique<RemoteInput::RemoteInputInjector>(std::move(sink));
}

void RemoteDesktopController::setSecureDesktopProbe(
    std::unique_ptr<RemoteDesktop::ISecureDesktopProbe> probe) {
    secureDesktopMonitor_ =
        std::make_unique<RemoteDesktop::SecureDesktopMonitor>(std::move(probe));
}

void RemoteDesktopController::startHostControlSide() {
    if (injector_) injector_->beginSession(hostSessionId_);
    if (hostWatchdogTimer_) hostWatchdogTimer_->start();
    // 共享期间别让系统自动锁屏：一锁就进安全桌面，远程彻底失联，
    // 而人在外面没法自己解。
    sleepBlocker_.acquire();
}

void RemoteDesktopController::stopHostControlSide() {
    if (hostWatchdogTimer_) hostWatchdogTimer_->stop();
    // endSession 内部会把按住的键鼠全部抬起。
    if (injector_) injector_->endSession();
    sleepBlocker_.release();
}

void RemoteDesktopController::tickHostWatchdogs(qint64 nowMs) {
    if (injector_) injector_->tickWatchdog(nowMs);

    if (!secureDesktopMonitor_ || hostState_ != HostState::Sharing) return;
    const auto change = secureDesktopMonitor_->poll(nowMs);
    if (change == RemoteDesktop::SecureDesktopMonitor::Change::None) return;

    // 播报给控制端：让对面能区分"在等系统授权"和"断网/崩溃"，
    // 而不是对着一块卡住的画面猜。
    Signal notice;
    notice.type = Type::Notice;
    notice.sessionId = hostSessionId_;
    notice.noticeCode = QString::fromLatin1(
        change == RemoteDesktop::SecureDesktopMonitor::Change::Entered
            ? RemoteDesktopSignals::NoticeCodes::kSecureDesktopEntered
            : RemoteDesktopSignals::NoticeCodes::kSecureDesktopLeft);
    send(hostPeerId_, notice);
}

void RemoteDesktopController::flushPendingInput(qint64 nowMs) {
    if (!engine_ || viewerState_ != ViewerState::Viewing) {
        // 状态没到 Viewing 时输入是攒着发不出去的。这一条专门为了区分
        // "对面没收到"和"我根本没发"——首帧没到达导致状态卡在 Connecting
        // 是踩过的坑，画面正常显示但一个包都发不出去。
        if (RemoteInput::traceEnabled() && inputSender_.isSessionActive()) {
            ++traceBlockedNotViewing_;
        }
        flushInputTrace(nowMs);
        return;
    }
    for (const auto& packet : inputSender_.flush(nowMs)) {
        const bool reliable = packet.channel == RemoteInput::Channel::Reliable;
        const bool ok = engine_->sendCustomMessage(
            reliable ? RemoteInput::kCmdIdReliable : RemoteInput::kCmdIdUnreliable,
            packet.payload, reliable, reliable);
        if (RemoteInput::traceEnabled()) {
            // SDK 的返回值以前是丢掉的。它为假意味着 TRTC 自己把包拒了
            // （限频、角色不对、没在房间里），和"发出去但对面没收到"是
            // 完全不同的两回事，必须分开记。
            if (ok) {
                ++traceSentOk_;
            } else {
                ++traceSentRejected_;
            }
        }
    }
    flushInputTrace(nowMs);
}

void RemoteDesktopController::flushInputTrace(qint64 nowMs) {
    if (!RemoteInput::traceEnabled()) return;
    if (traceWindowStartMs_ == 0) traceWindowStartMs_ = nowMs;
    if (nowMs - traceWindowStartMs_ < 1000) return;
    traceWindowStartMs_ = nowMs;

    const bool viewerBusy = traceSentOk_ > 0 || traceSentRejected_ > 0
                            || traceBlockedNotViewing_ > 0;
    const bool hostBusy = traceRecvPackets_ > 0 || traceRecvBadPayload_ > 0
                          || traceRecvDenied_ > 0;

    if (!viewerBusy && !hostBusy) {
        // 完全没动静时也得定期报个到，否则"日志里什么都没有"会同时对应
        // 「没收到包」和「日志压根没开」两种情况，等于白记。
        // 但也不能每秒一行把有用的信息淹掉，所以降到 5 秒一次。
        if (++traceQuietWindows_ < 5) return;
        traceQuietWindows_ = 0;
        if (hostState_ == RemoteDesktop::HostState::Sharing) {
            RemoteInput::trace(
                QStringLiteral("[被控端] 共享中，近 5 秒没有收到任何输入包"
                               "（允许控制=%1，对端=%2）")
                    .arg(settings_.allowRemoteControl ? QStringLiteral("是") : QStringLiteral("否"))
                    .arg(hostPeerId_.isEmpty() ? QStringLiteral("(空)") : hostPeerId_));
        }
        if (viewerState_ == ViewerState::Viewing) {
            RemoteInput::trace(QStringLiteral("[控制端] 画面已连接，近 5 秒没有产生任何输入"));
        }
        return;
    }
    traceQuietWindows_ = 0;

    if (viewerBusy) {
        RemoteInput::trace(
            QStringLiteral("[控制端] 已发=%1 被SDK拒=%2 未就绪未发=%3 viewerState=%4")
                .arg(traceSentOk_)
                .arg(traceSentRejected_)
                .arg(traceBlockedNotViewing_)
                .arg(static_cast<int>(viewerState_)));
    }
    if (hostBusy) {
        RemoteInput::trace(
            QStringLiteral("[被控端] 收包=%1 坏包=%2 被门禁拒=%3(%4) 已注入事件=%5 hostState=%6")
                .arg(traceRecvPackets_)
                .arg(traceRecvBadPayload_)
                .arg(traceRecvDenied_)
                .arg(QLatin1String(RemoteDesktop::inputVerdictName(traceLastVerdict_)))
                .arg(traceInjectedEvents_)
                .arg(static_cast<int>(hostState_)));
    }

    traceSentOk_ = 0;
    traceSentRejected_ = 0;
    traceBlockedNotViewing_ = 0;
    traceRecvPackets_ = 0;
    traceRecvBadPayload_ = 0;
    traceRecvDenied_ = 0;
    traceInjectedEvents_ = 0;
}

void RemoteDesktopController::handleCustomMessage(const QString& fromUserId, int cmdId,
                                                  const QByteArray& payload) {
    if (injector_ == nullptr) return;
    if (cmdId != RemoteInput::kCmdIdReliable && cmdId != RemoteInput::kCmdIdUnreliable) return;

    // 计数放在解包之前：能走到这里就说明 TRTC 的消息确实到岸了，
    // 这正是"对面到底发没发过来"的分水岭。
    if (RemoteInput::traceEnabled()) ++traceRecvPackets_;

    RemoteInput::Packet packet;
    if (!RemoteInput::decodePacket(payload, &packet)) {
        if (RemoteInput::traceEnabled()) ++traceRecvBadPayload_;
        return;
    }

    // 门禁在状态机里，这里只执行结论——"什么情况下别人能操作我的电脑"
    // 必须只有一个地方需要读。
    const auto verdict = RemoteDesktop::remoteInputVerdict(
        hostState_, settings_.allowRemoteControl, hostSessionId_, hostPeerId_, packet.sessionId,
        fromUserId);
    if (verdict != RemoteDesktop::InputVerdict::Accepted) {
        if (RemoteInput::traceEnabled()) {
            ++traceRecvDenied_;
            traceLastVerdict_ = verdict;
        }
        return;
    }

    const auto channel = cmdId == RemoteInput::kCmdIdReliable ? RemoteInput::Channel::Reliable
                                                             : RemoteInput::Channel::Unreliable;
    const bool applied =
        injector_->handlePacket(packet, channel, QDateTime::currentMSecsSinceEpoch());
    if (RemoteInput::traceEnabled()) {
        traceLastVerdict_ = verdict;
        // 门禁放行了不等于真注入了：序号乱序/重复的包会在注入器里被丢掉。
        if (applied) traceInjectedEvents_ += packet.events.size();
    }
}

void RemoteDesktopController::setIdGenerator(IdGenerator generator) {
    if (generator) idGenerator_ = std::move(generator);
}

void RemoteDesktopController::setRemoteVideoHandler(ITrtcEngine::RemoteVideoCallback handler) {
    remoteVideoHandler_ = std::move(handler);
    if (!engine_) return;
    // 不能把 handler 直接塞给引擎：首帧到达是「Connecting → Viewing」的唯一
    // 触发点，而输入会话正是跟着 Viewing 开的。直接转发的话状态机永远停在
    // Connecting，画面照常显示、控制却一个包都发不出去（实测踩过）。
    engine_->setRemoteVideoCallback([this](const QString& userId, bool available) {
        if (available) setViewerState(viewerOnFirstFrame(viewerState_).nextState);
        if (remoteVideoHandler_) remoteVideoHandler_(userId, available);
    });
}

void RemoteDesktopController::setErrorHandler(ITrtcEngine::ErrorCallback handler) {
    if (engine_) engine_->setErrorCallback(std::move(handler));
}

void RemoteDesktopController::bindRemoteView(const QString& userId, void* renderWindow) {
    if (engine_) engine_->bindRemoteView(userId, renderWindow);
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
                if (!engine_ || !engine_->startViewing(roomParams(viewerRoomId_), nullptr)) {
                    setViewerState(ViewerState::Failed, QStringLiteral("远程画面启动失败"));
                    return true;
                }
            }
            setViewerState(transition.nextState, transition.failureReason);
            return true;
        }
        case Type::Notice: {
            // 只认当前会话的播报，忽略过期会话的迟到消息。
            if (signal.sessionId.isEmpty() || signal.sessionId != viewerSessionId_) return true;
            emit peerNoticeReceived(signal.noticeCode);
            return true;
        }
        case Type::Stop: {
            if (!viewerSessionId_.isEmpty() && signal.sessionId == viewerSessionId_) {
                setViewerState(viewerOnSignal(viewerState_, signal).nextState);
                engine_->stop();
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
    input.passwordConfigured = settings_.hasPassword();
    // 密码校验只在设了密码时才有意义；proof 绑定本次会话参数。
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
                                    && input.passwordConfigured
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
            startHostControlSide();
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
            // 先收控制侧再停引擎：注入器要在这里把按住的键全抬了，
            // 否则会话没了而 Ctrl 还按着，人不在电脑旁没法自己解。
            stopHostControlSide();
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
        Signal stop;
        stop.type = Type::Stop;
        stop.sessionId = viewerSessionId_;
        setViewerState(viewerOnLocalClose(viewerState_).nextState);
        engine_->stop();
        send(viewerPeerId_, stop);
        viewerSessionId_.clear();
        viewerRoomId_.clear();
        viewerPeerId_.clear();
    }
}

void RemoteDesktopController::setViewerState(ViewerState state, const QString& failureReason) {
    if (state == viewerState_ && failureReason.isEmpty()) return;

    if (viewerState_ == ViewerState::Viewing && state != ViewerState::Viewing) {
        inputSender_.queueReleaseAll();
        flushPendingInput(QDateTime::currentMSecsSinceEpoch());
    }
    viewerState_ = state;

    // 输入会话跟着观看状态走：画面到了才开始收输入，会话一结束立刻清空
    // 攒着没发的事件——否则下一场会话会把上一场的残留输入吐出去。
    if (state == ViewerState::Viewing) {
        inputSender_.beginSession(viewerSessionId_);
    } else {
        inputSender_.endSession();
    }
    emit viewerStateChanged(state, failureReason);
}
