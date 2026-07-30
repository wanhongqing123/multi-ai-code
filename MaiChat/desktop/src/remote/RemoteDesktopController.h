#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <memory>

#include "remote/DisplaySleepBlocker.h"
#include "remote/RemoteDesktopSession.h"
#include "remote/RemoteDesktopSettings.h"
#include "remote/RemoteDesktopSignal.h"
#include "remote/RemoteInputInjector.h"
#include "remote/RemoteInputSender.h"
#include "remote/SecureDesktopMonitor.h"
#include "remote/TrtcEngine.h"

class QTimer;

// 把信令、状态机、TRTC 引擎与设置粘起来的一层。
//
// 只做三件事：翻译（IM 文本 ↔ 信令）、决策委派（交给状态机）、执行副作用
// （进房/推流/回信令/通知 UI）。所有判断逻辑都在状态机里，这里不额外做安全决策，
// 便于用 fake 完整覆盖。
class RemoteDesktopController : public QObject {
    Q_OBJECT

public:
    using SignalSendCompletion = std::function<void()>;
    // 发送信令用的回调（实际由 RemoteIMClient::sendText 承接）。完成回调用于
    // 应用退出时等待 stop 已交给 IM SDK，避免进程先结束导致对端收不到。
    using SendSignal = std::function<void(const QString& peerId,
                                          const QString& text,
                                          SignalSendCompletion completion)>;
    // 生成随机 id；测试注入固定值以获得确定性。
    using IdGenerator = std::function<QString()>;

    struct Config {
        int sdkAppId = 0;
        QString localUserId;
        // 由 TencentUserSigGenerator 生成，与 IM 共用。
        std::function<QString(const QString& userId)> userSigProvider;
    };

    RemoteDesktopController(Config config,
                            RemoteDesktopSettings settings,
                            std::unique_ptr<RemoteDesktop::ITrtcEngine> engine,
                            SendSignal sendSignal,
                            QObject* parent = nullptr);
    ~RemoteDesktopController() override;

    // 收到任意 IM 文本时调用。返回 true 表示这是远程桌面信令、已被消费，
    // 调用方不应再把它当普通消息入库或展示。
    bool handleIncomingText(const QString& fromUserId, const QString& text);

    // 控制端：请求查看 peer 的屏幕。password 为空表示不带 proof
    // （对方是有人值守模式时无需密码）。
    void requestView(const QString& peerId, const QString& password = QString());

    // 被控端：有人值守弹窗的结果。
    void resolveConsent(bool accepted);

    // 两端通用：主动结束当前会话。先向对端发送 stop，再清理本地会话；
    // completion 在所有 stop 发送完成且本地清理完成后调用。
    void stopSession(SignalSendCompletion completion = {});

    // 远端画面可用性 / 引擎错误的转发口。UI 据此绑定渲染窗口与提示。
    void setRemoteVideoHandler(RemoteDesktop::ITrtcEngine::RemoteVideoCallback handler);
    // 远端画面真实尺寸。控制端必须据此算黑边，否则鼠标坐标会整体偏移。
    void setRemoteVideoSizeHandler(RemoteDesktop::ITrtcEngine::RemoteVideoSizeCallback handler);
    void setErrorHandler(RemoteDesktop::ITrtcEngine::ErrorCallback handler);
    void bindRemoteView(const QString& userId, void* renderWindow);

    RemoteDesktop::HostState hostState() const;
    RemoteDesktop::ViewerState viewerState() const;
    QString viewerPeerId() const;
    const RemoteDesktopSettings& settings() const;
    void updateSettings(const RemoteDesktopSettings& settings);

    void setIdGenerator(IdGenerator generator);

    // ---- 远程控制 ----

    // 控制端：把采集到的输入交进来。会话未建立时是空操作。
    RemoteInput::RemoteInputSender& inputSender() { return inputSender_; }
    // 控制端：定时把攒下的输入发出去。默认由内部定时器驱动，
    // 测试可注入时刻直接调。
    void flushPendingInput(qint64 nowMs);

    // 被控端：注入器可替换，测试用 Fake 断言"到底动没动鼠标"。
    void setInputSink(std::unique_ptr<RemoteInput::IRemoteInputSink> sink);
    // 被控端：安全桌面探针可替换，测试无需真的弹 UAC。
    void setSecureDesktopProbe(std::unique_ptr<RemoteDesktop::ISecureDesktopProbe> probe);
    // 被控端：驱动看门狗与安全桌面轮询。默认由内部定时器驱动。
    void tickHostWatchdogs(qint64 nowMs);

    bool isRemoteControlAllowed() const { return settings_.allowRemoteControl; }

signals:
    // 控制端收到被控端的状态播报（当前只有安全桌面进出）。
    void peerNoticeReceived(const QString& noticeCode);

signals:
    // 被控端需要弹确认窗（有人值守）。
    void consentRequested(const QString& fromUserId);
    // 被控端开始/结束共享——UI 据此显示或撤下常驻指示条。
    void sharingStarted(const QString& peerUserId);
    void sharingStopped();
    // 无人值守连续失败触发的模式降级，UI 应提示用户。
    void modeDowngraded();
    // 控制端状态变化；failureReason 仅在失败时非空。
    void viewerStateChanged(RemoteDesktop::ViewerState state, const QString& failureReason);
    // 设置被控制器改写（失败计数、模式降级），宿主应据此持久化。
    void settingsChanged(const RemoteDesktopSettings& settings);

private:
    void send(const QString& peerId,
              const RemoteDesktopSignals::Signal& signal,
              SignalSendCompletion completion = {});
    void handleInvite(const QString& fromUserId, const RemoteDesktopSignals::Signal& signal);
    // 密码尝试的计数与降级。单独抽出来，避免这段安全语义混在信令分发里。
    void recordAuthAttempt(const RemoteDesktop::HostInviteInput& input,
                           const RemoteDesktop::HostDecision& decision);
    void applyHostDecision(const RemoteDesktop::HostDecision& decision,
                           const QString& peerId,
                           SignalSendCompletion completion = {});
    void setViewerState(RemoteDesktop::ViewerState state, const QString& failureReason = QString());
    RemoteDesktop::TrtcRoomParams roomParams(const QString& roomId) const;
    void handleCustomMessage(const QString& fromUserId, int cmdId, const QByteArray& payload);
    void startHostControlSide();
    void stopHostControlSide();
    // 诊断日志每秒汇总一次输出。移动包一秒二十几个，一包一行会把日志冲垮，
    // 也看不出趋势。空闲时不产生任何输出，只有真在会话里才写。
    void flushInputTrace(qint64 nowMs);

    Config config_;
    RemoteDesktopSettings settings_;
    std::unique_ptr<RemoteDesktop::ITrtcEngine> engine_;
    SendSignal sendSignal_;
    IdGenerator idGenerator_;
    // UI 侧的远端画面回调。控制器要先截一道（首帧驱动状态机），再转交。
    RemoteDesktop::ITrtcEngine::RemoteVideoCallback remoteVideoHandler_;

    RemoteDesktop::HostState hostState_ = RemoteDesktop::HostState::Idle;
    RemoteDesktop::ViewerState viewerState_ = RemoteDesktop::ViewerState::Idle;

    // 被控端当前会话
    QString hostPeerId_;
    QString hostSessionId_;
    QString hostRoomId_;
    // 控制端当前会话
    QString viewerPeerId_;
    QString viewerSessionId_;
    QString viewerRoomId_;

    // 控制端：攒输入并按配额发出。
    RemoteInput::RemoteInputSender inputSender_;
    QTimer* inputFlushTimer_ = nullptr;

    // 被控端：注入、看门狗、安全桌面播报、阻止锁屏。
    std::unique_ptr<RemoteInput::RemoteInputInjector> injector_;
    std::unique_ptr<RemoteDesktop::SecureDesktopMonitor> secureDesktopMonitor_;
    QTimer* hostWatchdogTimer_ = nullptr;
    DisplaySleepBlocker sleepBlocker_;

    // ---- 诊断计数，每秒汇总一行进应用日志 ----
    qint64 traceWindowStartMs_ = 0;
    // 控制端：发出去多少、被 SDK 拒了多少、因状态没就绪压根没发多少。
    int traceSentOk_ = 0;
    int traceSentRejected_ = 0;
    int traceBlockedNotViewing_ = 0;
    // 被控端：收到多少包、解包失败多少、门禁拒了多少（附最后一次原因）、
    // 真正注入了多少个事件。
    int traceRecvPackets_ = 0;
    int traceRecvBadPayload_ = 0;
    int traceRecvDenied_ = 0;
    int traceInjectedEvents_ = 0;
    RemoteDesktop::InputVerdict traceLastVerdict_ = RemoteDesktop::InputVerdict::Accepted;
    // 连续多少个空窗口。攒够 5 个报一次到，免得"日志里什么都没有"分不清
    // 是没收到包还是日志没开。
    int traceQuietWindows_ = 0;
};
