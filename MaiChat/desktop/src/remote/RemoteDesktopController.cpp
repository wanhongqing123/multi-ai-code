#include "remote/RemoteDesktopController.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>

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
      idGenerator_(defaultId) {
    if (engine_) {
        engine_->setCustomMessageCallback(
            [this](const QString& userId, int cmdId, const QByteArray& payload) {
                handleCustomMessage(userId, cmdId, payload);
            });
        // 状态机回调必须跟控制器同生命周期，不能依赖 UI 何时注册观察者。
        // 否则进房期间先到的画面通知会丢失，状态永远停在 Connecting。
        engine_->setRemoteVideoCallback([this](const QString& userId, bool available) {
            if (available) setViewerState(viewerOnFirstFrame(viewerState_).nextState);
            if (remoteVideoHandler_) remoteVideoHandler_(userId, available);
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
    // 会话一开始就把被控端的屏幕几何写下来。它和控制端的 contentRect 是一对
    // 必须能对上的数字，缺任何一半都没法判断偏移出在哪一端。
    qInfo().noquote() << QStringLiteral("[remote-input] host geometry: %1  allowRemoteControl=%2")
                             .arg(RemoteInput::describeInjectionGeometry())
                             .arg(settings_.allowRemoteControl ? QStringLiteral("true")
                                                               : QStringLiteral("false"));
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
        if (inputSender_.isSessionActive()) ++traceBlockedNotViewing_;
        flushInputTrace(nowMs);
        return;
    }
    for (const auto& packet : inputSender_.flush(nowMs)) {
        const bool reliable = packet.channel == RemoteInput::Channel::Reliable;
        // SDK 的返回值以前是丢掉的。它为假意味着 TRTC 自己把包拒了（限频、
        // 角色不对、没在房间里），和"发出去但对面没收到"是完全不同的两回事，
        // 必须分开记。
        if (engine_->sendCustomMessage(
                reliable ? RemoteInput::kCmdIdReliable : RemoteInput::kCmdIdUnreliable,
                packet.payload, reliable, reliable)) {
            ++traceSentOk_;
        } else {
            ++traceSentRejected_;
        }
    }
    flushInputTrace(nowMs);
}

void RemoteDesktopController::flushInputTrace(qint64 nowMs) {
    if (traceWindowStartMs_ == 0) traceWindowStartMs_ = nowMs;
    if (nowMs - traceWindowStartMs_ < 1000) return;
    traceWindowStartMs_ = nowMs;

    const bool viewerBusy = traceSentOk_ > 0 || traceSentRejected_ > 0
                            || traceBlockedNotViewing_ > 0;
    const bool hostBusy = traceRecvPackets_ > 0 || traceRecvBadPayload_ > 0
                          || traceRecvDenied_ > 0;

    if (!viewerBusy && !hostBusy) {
        // 完全没动静时也得定期报个到，否则"日志里什么都没有"会同时对应
        // 「没收到包」和「这段时间根本没在会话里」两种情况，等于白记。
        // 但也不能每秒一行把有用的信息淹掉，所以降到 5 秒一次。
        if (++traceQuietWindows_ < 5) return;
        traceQuietWindows_ = 0;
        if (hostState_ == RemoteDesktop::HostState::Sharing) {
            qInfo().noquote()
                << QStringLiteral(
                       "[remote-input] host: sharing, no input packet in the last 5s "
                       "(allowRemoteControl=%1, peer=%2)")
                       .arg(settings_.allowRemoteControl ? QStringLiteral("true")
                                                         : QStringLiteral("false"))
                       .arg(hostPeerId_.isEmpty() ? QStringLiteral("<empty>") : hostPeerId_);
        }
        if (viewerState_ == ViewerState::Viewing) {
            qInfo().noquote() << QStringLiteral(
                "[remote-input] viewer: stream connected, no input produced in the last 5s");
        }
        return;
    }
    traceQuietWindows_ = 0;

    if (viewerBusy) {
        qInfo().noquote()
            << QStringLiteral(
                   "[remote-input] viewer: sent=%1 rejected-by-sdk=%2 blocked-not-viewing=%3 "
                   "state=%4")
                   .arg(traceSentOk_)
                   .arg(traceSentRejected_)
                   .arg(traceBlockedNotViewing_)
                   .arg(QLatin1String(RemoteDesktop::viewerStateName(viewerState_)));
    }
    if (hostBusy) {
        // 带上最后注入的归一化坐标：和控制端同一秒的样本一比，就知道是传输
        // 途中变了、还是两端算法不一致。
        QString sample;
        if (injector_ && injector_->hasLastMove()) {
            sample = QStringLiteral(" last-move=(%1,%2)")
                         .arg(injector_->lastMoveX(), 0, 'f', 4)
                         .arg(injector_->lastMoveY(), 0, 'f', 4);
        }
        qInfo().noquote()
            << QStringLiteral(
                   "[remote-input] host: received=%1 malformed=%2 denied=%3(%4) "
                   "injected-events=%5 state=%6%7")
                   .arg(traceRecvPackets_)
                   .arg(traceRecvBadPayload_)
                   .arg(traceRecvDenied_)
                   .arg(QLatin1String(RemoteDesktop::inputVerdictName(traceLastVerdict_)))
                   .arg(traceInjectedEvents_)
                   .arg(QLatin1String(RemoteDesktop::hostStateName(hostState_)))
                   .arg(sample);
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
    ++traceRecvPackets_;

    RemoteInput::Packet packet;
    if (!RemoteInput::decodePacket(payload, &packet)) {
        ++traceRecvBadPayload_;
        return;
    }

    // 门禁在状态机里，这里只执行结论——"什么情况下别人能操作我的电脑"
    // 必须只有一个地方需要读。
    const auto verdict = RemoteDesktop::remoteInputVerdict(
        hostState_, settings_.allowRemoteControl, hostSessionId_, hostPeerId_, packet.sessionId,
        fromUserId);
    traceLastVerdict_ = verdict;
    if (verdict != RemoteDesktop::InputVerdict::Accepted) {
        ++traceRecvDenied_;
        return;
    }

    const auto channel = cmdId == RemoteInput::kCmdIdReliable ? RemoteInput::Channel::Reliable
                                                             : RemoteInput::Channel::Unreliable;
    // 门禁放行了不等于真注入了：序号乱序/重复的包会在注入器里被丢掉。
    if (injector_->handlePacket(packet, channel, QDateTime::currentMSecsSinceEpoch())) {
        traceInjectedEvents_ += packet.events.size();
    }
}

void RemoteDesktopController::setIdGenerator(IdGenerator generator) {
    if (generator) idGenerator_ = std::move(generator);
}

void RemoteDesktopController::setRemoteVideoHandler(ITrtcEngine::RemoteVideoCallback handler) {
    remoteVideoHandler_ = std::move(handler);
}

void RemoteDesktopController::setRemoteVideoSizeHandler(
    ITrtcEngine::RemoteVideoSizeCallback handler) {
    if (!engine_) return;
    engine_->setRemoteVideoSizeCallback(
        [this, handler = std::move(handler)](const QString& userId, int width, int height) {
            // 记进日志：坐标偏移这类问题，光看现象分不清是"尺寸拿错了"还是
            // "映射算错了"，把真实值写下来一眼就能对。
            qInfo().noquote() << QStringLiteral("[remote-input] remote video size: %1x%2 (user=%3)")
                                     .arg(width)
                                     .arg(height)
                                     .arg(userId.isEmpty() ? QStringLiteral("<empty>") : userId);
            if (handler) handler(userId, width, height);
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
QString RemoteDesktopController::viewerPeerId() const { return viewerPeerId_; }
const RemoteDesktopSettings& RemoteDesktopController::settings() const { return settings_; }

void RemoteDesktopController::updateSettings(const RemoteDesktopSettings& settings) {
    settings_ = settings;
}

void RemoteDesktopController::send(const QString& peerId,
                                   const Signal& signal,
                                   SignalSendCompletion completion) {
    if (!sendSignal_ || peerId.isEmpty()) {
        if (completion) completion();
        return;
    }
    const QString text = RemoteDesktopSignals::encodeSignal(signal);
    if (text.isEmpty()) {
        if (completion) completion();
        return;
    }
    sendSignal_(peerId, text, std::move(completion));
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
                // 先通知 UI 创建原生渲染窗口。TRTC 的回调可能在 startViewing
                // 内同步到达，晚切状态会让首帧落在 Inviting 并被永久忽略。
                setViewerState(transition.nextState, transition.failureReason);
                if (!engine_ || !engine_->startViewing(roomParams(viewerRoomId_), nullptr)) {
                    setViewerState(ViewerState::Failed, QStringLiteral("远程画面启动失败"));
                    return true;
                }
                return true;
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
                                                const QString& peerId,
                                                SignalSendCompletion completion) {
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
            Signal stop;
            stop.type = Type::Stop;
            stop.sessionId = hostSessionId_;
            // 先把结束命令交给 IM SDK，再退出 TRTC。应用关闭路径会等待这个
            // 发送回执，防止 SDK 尚未接收消息时进程就结束。
            send(peerId.isEmpty() ? hostPeerId_ : peerId, stop, std::move(completion));
            engine_->stop();
            hostState_ = decision.nextState;
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

void RemoteDesktopController::stopSession(SignalSendCompletion completion) {
    const bool stopHost = hostState_ == HostState::Sharing;
    const bool stopViewer = viewerState_ != ViewerState::Idle;
    const int sendCount = static_cast<int>(stopHost) + static_cast<int>(stopViewer);
    if (sendCount == 0) {
        if (completion) completion();
        return;
    }

    struct StopCompletionState {
        int pending = 0;
        bool cleanupDone = false;
        SignalSendCompletion completion;
    };
    auto state = std::make_shared<StopCompletionState>();
    state->pending = sendCount;
    state->completion = std::move(completion);
    const auto finishIfReady = [state] {
        if (!state->cleanupDone || state->pending != 0 || !state->completion) return;
        auto done = std::move(state->completion);
        done();
    };
    const auto signalSent = [state, finishIfReady] {
        --state->pending;
        finishIfReady();
    };

    if (stopHost) {
        applyHostDecision(decideOnPeerGone(hostState_), hostPeerId_, signalSent);
    }
    if (stopViewer) {
        Signal stop;
        stop.type = Type::Stop;
        stop.sessionId = viewerSessionId_;
        // 与被控端使用同一 sessionId，确保它能命中当前会话并停止共享。
        send(viewerPeerId_, stop, signalSent);
        setViewerState(viewerOnLocalClose(viewerState_).nextState);
        engine_->stop();
        viewerSessionId_.clear();
        viewerRoomId_.clear();
        viewerPeerId_.clear();
    }
    state->cleanupDone = true;
    finishIfReady();
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
