#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <memory>

#include "remote/RemoteDesktopSettings.h"
#include "remote/RemoteDesktopSignal.h"
#include "remote/TrtcEngine.h"

// 把信令、状态机、TRTC 引擎与设置粘起来的一层。
//
// 只做三件事：翻译（IM 文本 ↔ 信令）、决策委派（交给状态机）、执行副作用
// （进房/推流/回信令/通知 UI）。所有判断逻辑都在状态机里，这里不额外做安全决策，
// 便于用 fake 完整覆盖。
class RemoteDesktopController : public QObject {
    Q_OBJECT

public:
    // 发送信令用的回调（实际由 RemoteIMClient::sendText 承接）。
    using SendSignal = std::function<void(const QString& peerId, const QString& text)>;
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

    // 两端通用：主动结束当前会话。
    void stopSession();

    RemoteDesktop::HostState hostState() const;
    RemoteDesktop::ViewerState viewerState() const;
    const RemoteDesktopSettings& settings() const;
    void updateSettings(const RemoteDesktopSettings& settings);

    void setIdGenerator(IdGenerator generator);

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
    void send(const QString& peerId, const RemoteDesktopSignals::Signal& signal);
    void applyHostDecision(const RemoteDesktop::HostDecision& decision, const QString& peerId);
    void setViewerState(RemoteDesktop::ViewerState state, const QString& failureReason = QString());
    RemoteDesktop::TrtcRoomParams roomParams(const QString& roomId) const;

    Config config_;
    RemoteDesktopSettings settings_;
    std::unique_ptr<RemoteDesktop::ITrtcEngine> engine_;
    SendSignal sendSignal_;
    IdGenerator idGenerator_;

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
};
