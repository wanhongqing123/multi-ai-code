// 文件作用：声明桌面扫码登录窗口及其等待、失败、过期和取消状态。
// 类型关系：LoginDialog 继承 QDialog，异步结果来自 DesktopAuthClient。
// 对外边界：只在取得与账号一致的服务端 IM 凭据后接受对话框；不暴露手填 UserID。
#pragma once

#include <QDateTime>
#include <QDialog>
#include <QTimer>

#include "auth/DesktopAuthClient.h"

class QLabel;
class QPushButton;

class LoginDialog final : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(DesktopAuthClient* auth = nullptr, QWidget* parent = nullptr);
    void start();
    void reject() override;

private:
    enum class State { Restoring, RestoreError, Loading, Waiting, Scanned, Expired,
                       Denied, ApprovalRevoked, NetworkError, Connecting,
                       CredentialError };
    void buildUi();
    void applyStyle();
    void setState(State state, const QString& detail = {});
    void requestQr();
    void continueQrRequest();
    void pollClaim();
    void updateCountdown();
    void retry();
    void finishCancellation();
    void cancelRequest(const DesktopLoginRequest& request);
    void revokeUnwantedClaim(const QJsonObject& session);

    DesktopAuthClient* auth_ = nullptr;
    DesktopLoginRequest request_;
    QDateTime expiresAt_;
    QTimer countdownTimer_;
    QTimer pollTimer_;
    quint64 generation_ = 0;
    bool pollInFlight_ = false;
    bool closing_ = false;
    bool cancellationSetupComplete_ = false;
    bool qrRestartPending_ = false;
    int pendingClaimCount_ = 0;
    int pendingCancellationCount_ = 0;
    int pendingRevocationCount_ = 0;
    int pendingLogoutCount_ = 0;
    State state_ = State::Loading;
    QLabel* qrLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    QLabel* codeLabel_ = nullptr;
    QLabel* countdownLabel_ = nullptr;
    QPushButton* retryButton_ = nullptr;
};
