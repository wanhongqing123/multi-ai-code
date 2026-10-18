// 文件作用：桌面端账号服务客户端，负责扫码、会话续期、前台校验及 IM/模型临时凭据。
// 类型关系：DesktopAuthClient 继承 QObject；DesktopLoginRequest 和 DesktopImCredential 是只承载结果的数据结构。
// 对外边界：刷新令牌交给系统安全存储；访问令牌、verifier、UserSig 和模型密钥只在进程内使用。
// 直接依赖：DesktopCurlAuthTransport、SecureRefreshTokenStore；不依赖登录窗口或 IM SDK。
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QMutex>
#include <QPair>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>

#include "auth/SecureRefreshTokenStore.h"

struct DesktopLoginRequest {
    QString requestId;
    QString verifier;
    QString displayCode;
    QByteArray qrPng;
    int expiresIn = 0;
};

struct DesktopImCredential {
    int sdkAppId = 0;
    QString userId;
    QString userSig;
    QDateTime expiresAt;
    bool isValid() const {
        return sdkAppId > 0 && !userId.isEmpty() && !userSig.isEmpty() &&
               expiresAt > QDateTime::currentDateTimeUtc();
    }
};

class DesktopAuthClient final : public QObject {
public:
    enum class ClaimStatus { Pending, Scanned, Denied, ApprovalRevoked, Expired,
                             Approved, Failed };
    struct ClaimResult {
        ClaimStatus status = ClaimStatus::Failed;
        QJsonObject session;
        QString error;
    };
    using Completion = std::function<void(bool, const QString&)>;

    explicit DesktopAuthClient(QObject* parent = nullptr,
                               QUrl baseUrl = QUrl(QStringLiteral("https://ichat.life/api/v1/")),
                               bool useSecureStore = true);
    ~DesktopAuthClient() override;

    void restore(Completion completion);
    bool hasRestorableSession() const;
    void prepareForQrLogin();
    void createLoginRequest(std::function<void(const DesktopLoginRequest&, const QString&)> completion);
    void claim(const QString& requestId, const QString& verifier,
               std::function<void(const ClaimResult&)> completion);
    void cancelLoginRequest(const QString& requestId, const QString& verifier,
                            std::function<void()> completion = {});
    void revokeUnwantedClaim(const QJsonObject& session,
                             std::function<void()> completion = {});
    void adoptApprovedSession(const QJsonObject& session, Completion completion);
    void fetchImCredential(Completion completion);
    // 账号鉴权 GET：仅把 GLM/DeepSeek Key 交给调用方内存，不写本机存储。
    using AccountModelKeysCompletion =
        std::function<void(const QHash<QString, QString>&, const QString&)>;
    void fetchAccountModelKeys(AccountModelKeysCompletion completion);
    using FriendQrCompletion = std::function<void(const QString&, const QByteArray&, const QString&)>;
    void fetchFriendQr(FriendQrCompletion completion);
    void startSessionMonitoring();
    void checkSessionNow();
    void logout(Completion completion);
    void startModelCredentialMonitoring();
    void refreshModelCredentialsNow(bool force = false);
    std::function<QString()> arkApiKeyProvider() const;
    std::function<QPair<QString, QString>()> bailianCredentialProvider() const;
    std::function<QPair<QString, QString>()> mediaServiceAccessProvider() const;
    DesktopImCredential imCredential() const { return imCredential_; }
    QString authenticatedUserId() const { return userId_; }
    void setCredentialUpdatedHandler(std::function<void(const DesktopImCredential&)> handler);
    void setSessionInvalidatedHandler(std::function<void(const QString&)> handler);

private:
    struct HttpResult {
        int status = 0;
        QJsonObject body;
        QString error;
    };
    void request(const QString& path, const QByteArray& method, const QJsonObject& body,
                 const QString& bearer, std::function<void(const HttpResult&)> completion);
    void refresh(Completion completion);
    void fetchImCredentialImpl(Completion completion, bool retryAfterUnauthorized);
    void fetchAccountModelKeysImpl(AccountModelKeysCompletion completion,
                                bool retryAfterUnauthorized);
    void fetchFriendQrImpl(FriendQrCompletion completion, bool retryAfterUnauthorized);
    bool adoptSession(const QJsonObject& body, QString* error);
    void scheduleRefresh();
    void scheduleCredentialRefresh();
    void invalidateSession(const QString& reason);
    void clearLocalSession();
    void fetchModelCredential(bool bailian, bool retryAfterUnauthorized);
    void clearModelCredentials();
    QString responseError(const HttpResult& result) const;

    std::vector<std::shared_ptr<std::atomic_bool>> pendingRequests_;
    SecureRefreshTokenStore secureStore_;
    bool useSecureStore_ = true;
    QUrl baseUrl_;
    QString accessToken_;
    QString refreshToken_;
    QString userId_;
    QDateTime accessExpiresAt_;
    DesktopImCredential imCredential_;
    QTimer refreshTimer_;
    QTimer credentialTimer_;
    QTimer livenessTimer_;
    QTimer modelCredentialTimer_;
    bool refreshInFlight_ = false;
    bool livenessInFlight_ = false;
    bool monitoringSession_ = false;
    bool foregroundHookInstalled_ = false;
    bool loggingOut_ = false;
    bool monitoringModelCredentials_ = false;
    bool arkRequestInFlight_ = false;
    bool bailianRequestInFlight_ = false;
    bool arkAccessDenied_ = false;
    bool bailianAccessDenied_ = false;
    struct ModelCredentialCache {
        QMutex mutex;
        QString arkKey;
        qint64 arkExpiresAt = 0;
        QString bailianKey;
        QString wanWorkspaceId;
        qint64 bailianExpiresAt = 0;
    };
    std::shared_ptr<ModelCredentialCache> modelCredentials_ =
        std::make_shared<ModelCredentialCache>();
    struct AccountAccessCache {
        QMutex mutex;
        QString token;
        qint64 expiresAt = 0;
    };
    std::shared_ptr<AccountAccessCache> accountAccess_ =
        std::make_shared<AccountAccessCache>();
    std::vector<Completion> refreshCallbacks_;
    std::function<void(const DesktopImCredential&)> credentialUpdated_;
    std::function<void(const QString&)> sessionInvalidated_;
};
