// 文件作用：实现桌面扫码授权、令牌轮换、前台会话检查及服务端签发的 IM/模型临时凭据获取。
// 主要入口：createLoginRequest、claim、restore、fetchImCredential、startSessionMonitoring 和模型续领。
// 实现边界：验证账号与设备槽位；所有网络请求禁止重定向与缓存，回复大小受限，不记录敏感令牌。
// 直接依赖：DesktopCurlAuthTransport、SecureRefreshTokenStore；界面状态由 LoginDialog 决定。
#include "auth/DesktopAuthClient.h"
#include "auth/DesktopCurlAuthTransport.h"

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtGlobal>
#include <QtConcurrent>
#include <algorithm>
#include <memory>
#include <utility>

namespace {

constexpr int kMaxAuthResponseBytes = 1024 * 1024;
constexpr int kSessionCheckIntervalMs = 30 * 1000;
constexpr int kCredentialRenewBeforeExpirySeconds = 6 * 60 * 60;
constexpr int kModelCredentialCheckIntervalMs = 30 * 1000;
constexpr int kModelCredentialRenewBeforeExpirySeconds = 90;

QByteArray base64Url(const QByteArray& bytes) {
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool validToken(const QString& token) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{43}$"));
    return pattern.match(token).hasMatch();
}

bool validModelApiKey(const QString& key) {
    const QByteArray bytes = key.toUtf8();
    if (bytes.isEmpty() || bytes.size() > 4096) return false;
    return std::all_of(key.cbegin(), key.cend(), [](QChar character) {
        return character.unicode() >= 0x20 && character.unicode() != 0x7f;
    });
}

bool validRequestId(const QString& requestId) {
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(requestId).hasMatch();
}

bool validDisplayCode(const QString& code) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{6}$"));
    return pattern.match(code).hasMatch();
}

QString platformName() {
#ifdef Q_OS_MACOS
    return QStringLiteral("mac");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return {};
#endif
}

}  // namespace

DesktopAuthClient::DesktopAuthClient(QObject* parent, QUrl baseUrl,
                                     bool useSecureStore)
    : QObject(parent), useSecureStore_(useSecureStore),
      baseUrl_(std::move(baseUrl)) {
    refreshTimer_.setSingleShot(true);
    credentialTimer_.setSingleShot(true);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh({}); });
    connect(&credentialTimer_, &QTimer::timeout, this, [this] {
        if (imCredential_.expiresAt >
            QDateTime::currentDateTimeUtc().addSecs(kCredentialRenewBeforeExpirySeconds)) {
            scheduleCredentialRefresh();
        } else {
            fetchImCredential({});
        }
    });
    livenessTimer_.setInterval(kSessionCheckIntervalMs);
    connect(&livenessTimer_, &QTimer::timeout, this, [this] {
        if (!qGuiApp || qGuiApp->applicationState() == Qt::ApplicationActive)
            checkSessionNow();
    });
    modelCredentialTimer_.setInterval(kModelCredentialCheckIntervalMs);
    connect(&modelCredentialTimer_, &QTimer::timeout,
            this, [this] { refreshModelCredentialsNow(); });
}

DesktopAuthClient::~DesktopAuthClient() {
    // 窗口关闭后停止未完成请求；工作线程只持有数据副本，不再访问本对象。
    for (const auto& request : pendingRequests_) request->store(true);
    clearModelCredentials();
}

// 所有账号请求走此入口。耗时的 libcurl 调用在后台执行，窗口线程只解析最终结果。
// 完成回调始终在此对象的 Qt 线程执行；窗口销毁后停止传输并丢弃过期回调。
void DesktopAuthClient::request(const QString& path, const QByteArray& method, const QJsonObject& body,
                                const QString& bearer, std::function<void(const HttpResult&)> completion) {
    const QUrl url = baseUrl_.resolved(QUrl(path));
    const QByteArray payload = method == "POST" ? QJsonDocument(body).toJson(QJsonDocument::Compact) : QByteArray();
    const QByteArray authorization = bearer.toUtf8();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    pendingRequests_.push_back(cancelled);
    auto* watcher = new QFutureWatcher<DesktopCurlAuthResult>(this);
    connect(watcher, &QFutureWatcher<DesktopCurlAuthResult>::finished, this,
            [this, watcher, cancelled, completion = std::move(completion)] {
                const DesktopCurlAuthResult response = watcher->result();
                pendingRequests_.erase(std::remove(pendingRequests_.begin(), pendingRequests_.end(), cancelled),
                                       pendingRequests_.end());
                watcher->deleteLater();
                HttpResult result;
                result.status = response.status;
                QJsonParseError parseError;
                QJsonDocument json;
                if (!response.responseTooLarge && response.body.size() <= kMaxAuthResponseBytes) {
                    json = QJsonDocument::fromJson(response.body, &parseError);
                    if (parseError.error == QJsonParseError::NoError && json.isObject()) result.body = json.object();
                }
                if (response.responseTooLarge || response.body.size() > kMaxAuthResponseBytes) {
                    result.status = 0;
                    result.error = QStringLiteral("登录服务返回的数据过大");
                } else if (!response.error.isEmpty()) {
                    result.error = response.error;
                    result.status = 0;
                } else if (!json.isObject()) {
                    result.error = QStringLiteral("登录服务返回了无效数据");
                }
                completion(result);
            });
    watcher->setFuture(QtConcurrent::run([url, method, payload, authorization, cancelled] {
        return DesktopCurlAuthTransport::perform(url, method, payload, authorization, cancelled);
    }));
}

QString DesktopAuthClient::responseError(const HttpResult& result) const {
    if (result.status == 429) return QStringLiteral("请求太频繁，请稍后再试");
    if (result.body.value(QStringLiteral("error")).toString() ==
        QStringLiteral("Invalid nickname")) return QStringLiteral("昵称格式不符合要求");
    if (result.status == 401 &&
        result.body.value(QStringLiteral("code")).toString() == QStringLiteral("SESSION_REPLACED"))
        return QStringLiteral("此电脑的登录已被另一台设备替换，请重新扫码登录");
    if (result.status == 401) return QStringLiteral("登录已失效，请重新扫码");
    if (result.status == 403) return QStringLiteral("手机拒绝了这次登录");
    if (!result.error.isEmpty()) return QStringLiteral("网络连接失败：%1").arg(result.error);
    return QStringLiteral("登录服务暂时不可用（%1）").arg(result.status);
}

bool DesktopAuthClient::adoptSession(const QJsonObject& body, QString* error) {
    const QString access = body.value(QStringLiteral("access_token")).toString();
    const QString refresh = body.value(QStringLiteral("refresh_token")).toString();
    const QString userId = body.value(QStringLiteral("user")).toObject()
                               .value(QStringLiteral("im_user_id")).toString();
    const QString accountId = body.value(QStringLiteral("user")).toObject()
                                  .value(QStringLiteral("id")).toString();
    const QString nickname = body.value(QStringLiteral("user")).toObject()
                                 .value(QStringLiteral("nickname")).toString();
    const QString slot = body.value(QStringLiteral("device_slot")).toString();
    const int expiresIn = body.value(QStringLiteral("expires_in")).toInt();
    if (!validToken(access) || !validToken(refresh) || userId.isEmpty() ||
        accountId.isEmpty() ||
        slot != platformName() || expiresIn <= 0) {
        if (error) *error = QStringLiteral("登录服务返回的账号凭据无效");
        return false;
    }
    accessToken_ = access;
    refreshToken_ = refresh;
    if (!userId_.isEmpty() && userId_ != userId) clearModelCredentials();
    userId_ = userId;
    accountId_ = accountId;
    nickname_ = nickname;
    accessExpiresAt_ = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
    {
        QMutexLocker lock(&accountAccess_->mutex);
        accountAccess_->token = access;
        accountAccess_->accountId = accountId;
        accountAccess_->expiresAt = accessExpiresAt_.toSecsSinceEpoch();
    }
    if (!loggingOut_ && useSecureStore_ && !secureStore_.write(refreshToken_)) {
        secureStore_.remove();
        qWarning("[auth] secure refresh token store unavailable; next launch requires QR login");
    }
    if (!loggingOut_) scheduleRefresh();
    return true;
}

void DesktopAuthClient::scheduleRefresh() {
    const qint64 delay = QDateTime::currentDateTimeUtc().msecsTo(accessExpiresAt_.addSecs(-60));
    refreshTimer_.start(int(qBound<qint64>(1000, delay, 15 * 60 * 1000)));
}

void DesktopAuthClient::scheduleCredentialRefresh() {
    const QDateTime deadline = imCredential_.expiresAt.addSecs(-kCredentialRenewBeforeExpirySeconds);
    const qint64 delay = QDateTime::currentDateTimeUtc().msecsTo(deadline);
    credentialTimer_.start(int(qBound<qint64>(60000, delay, 24 * 60 * 60 * 1000)));
}

void DesktopAuthClient::invalidateSession(const QString& reason) {
    const bool hadSession = !userId_.isEmpty();
    clearLocalSession();
    if (hadSession && sessionInvalidated_) sessionInvalidated_(reason);
}

void DesktopAuthClient::clearLocalSession() {
    refreshTimer_.stop();
    credentialTimer_.stop();
    livenessTimer_.stop();
    modelCredentialTimer_.stop();
    livenessInFlight_ = false;
    monitoringSession_ = false;
    monitoringModelCredentials_ = false;
    if (useSecureStore_) secureStore_.remove();
    accessToken_.clear();
    {
        QMutexLocker lock(&accountAccess_->mutex);
        accountAccess_->token.clear();
        accountAccess_->accountId.clear();
        accountAccess_->expiresAt = 0;
    }
    refreshToken_.clear();
    userId_.clear();
    accountId_.clear();
    nickname_.clear();
    imCredential_ = {};
    clearModelCredentials();
}

void DesktopAuthClient::clearModelCredentials() {
    arkAccessDenied_ = false;
    bailianAccessDenied_ = false;
    QMutexLocker lock(&modelCredentials_->mutex);
    modelCredentials_->arkKey.clear();
    modelCredentials_->arkExpiresAt = 0;
    modelCredentials_->bailianKey.clear();
    modelCredentials_->wanWorkspaceId.clear();
    modelCredentials_->bailianExpiresAt = 0;
}

std::function<QString()> DesktopAuthClient::arkApiKeyProvider() const {
    const auto cache = modelCredentials_;
    return [cache] {
        QMutexLocker lock(&cache->mutex);
        return cache->arkExpiresAt >
            QDateTime::currentSecsSinceEpoch() + 30 ? cache->arkKey : QString();
    };
}

std::function<QPair<QString, QString>()>
DesktopAuthClient::bailianCredentialProvider() const {
    const auto cache = modelCredentials_;
    return [cache] {
        QMutexLocker lock(&cache->mutex);
        if (cache->bailianExpiresAt <= QDateTime::currentSecsSinceEpoch() + 30)
            return QPair<QString, QString>();
        return qMakePair(cache->bailianKey, cache->wanWorkspaceId);
    };
}

std::function<QPair<QString, QString>()>
DesktopAuthClient::mediaServiceAccessProvider() const {
    QUrl mediaUrl = baseUrl_;
    mediaUrl.setPath(QStringLiteral("/maichat"));
    mediaUrl.setQuery(QString());
    mediaUrl.setFragment(QString());
    const QString base = mediaUrl.toString(QUrl::FullyEncoded);
    const auto cache = accountAccess_;
    return [base, cache] {
        QMutexLocker lock(&cache->mutex);
        if (cache->expiresAt <= QDateTime::currentSecsSinceEpoch() + 30)
            return QPair<QString, QString>();
        return qMakePair(base, cache->token);
    };
}

std::function<DesktopVideoCircleAccess()> DesktopAuthClient::videoCircleAccessProvider() const {
    const auto bases = desktopVideoCircleEndpointBases(baseUrl_);
    if (bases.first.isEmpty() || bases.second.isEmpty())
        return [] { return DesktopVideoCircleAccess{}; };
    const QString authBase = bases.first;
    const QString mediaBase = bases.second;
    const auto cache = accountAccess_;
    return [authBase, mediaBase, cache] {
        QMutexLocker lock(&cache->mutex);
        if (cache->expiresAt <= QDateTime::currentSecsSinceEpoch() + 30)
            return DesktopVideoCircleAccess{};
        DesktopVideoCircleAccess access;
        access.authBaseUrl = authBase;
        access.mediaBaseUrl = mediaBase;
        access.accountToken = cache->token;
        access.accountId = cache->accountId;
        return access;
    };
}

void DesktopAuthClient::startModelCredentialMonitoring() {
    if (loggingOut_ || monitoringModelCredentials_ || userId_.isEmpty()) return;
    monitoringModelCredentials_ = true;
    modelCredentialTimer_.start();
    refreshModelCredentialsNow();
}

void DesktopAuthClient::refreshModelCredentialsNow(bool force) {
    if (!monitoringModelCredentials_ || userId_.isEmpty()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool needArk = force;
    bool needBailian = force;
    {
        QMutexLocker lock(&modelCredentials_->mutex);
        needArk = needArk || modelCredentials_->arkExpiresAt <=
                    now + kModelCredentialRenewBeforeExpirySeconds;
        needBailian = needBailian || modelCredentials_->bailianExpiresAt <=
                        now + kModelCredentialRenewBeforeExpirySeconds;
    }
    if (needArk && !arkAccessDenied_) fetchModelCredential(false, true);
    if (needBailian && !bailianAccessDenied_) fetchModelCredential(true, true);
}

// Ark 与百炼独立续领；短期密钥只写线程安全的内存缓存，Agent 工具每次执行时读取。
// 401 只刷新账号令牌并重试一次，403 则停止本会话继续探测无权使用的模型能力。
void DesktopAuthClient::fetchModelCredential(bool bailian,
                                              bool retryAfterUnauthorized) {
    bool& inFlight = bailian ? bailianRequestInFlight_ : arkRequestInFlight_;
    if (inFlight || userId_.isEmpty()) return;
    inFlight = true;
    if (refreshInFlight_ || accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this, bailian, retryAfterUnauthorized](bool ok, const QString&) {
            (bailian ? bailianRequestInFlight_ : arkRequestInFlight_) = false;
            if (ok && monitoringModelCredentials_)
                fetchModelCredential(bailian, retryAfterUnauthorized);
        });
        return;
    }
    const QString path = bailian ? QStringLiteral("model/bailian-credential") :
                                   QStringLiteral("model/seedance-credential");
    request(path, "GET", {}, accessToken_,
            [this, bailian, retryAfterUnauthorized](const HttpResult& result) {
        (bailian ? bailianRequestInFlight_ : arkRequestInFlight_) = false;
        if (!monitoringModelCredentials_ || userId_.isEmpty()) return;
        if (result.status == 401 && retryAfterUnauthorized) {
            refresh([this, bailian](bool ok, const QString&) {
                if (ok && monitoringModelCredentials_)
                    fetchModelCredential(bailian, false);
            });
            return;
        }
        if (result.status == 401) {
            invalidateSession(responseError(result));
            return;
        }
        if (result.status == 403) {
            (bailian ? bailianAccessDenied_ : arkAccessDenied_) = true;
            QMutexLocker lock(&modelCredentials_->mutex);
            if (bailian) {
                modelCredentials_->bailianKey.clear();
                modelCredentials_->wanWorkspaceId.clear();
                modelCredentials_->bailianExpiresAt = 0;
            } else {
                modelCredentials_->arkKey.clear();
                modelCredentials_->arkExpiresAt = 0;
            }
            return;
        }
        if (result.status != 200) return;
        const QString key = result.body.value(QStringLiteral("api_key")).toString();
        const qint64 expiresAt = result.body.value(QStringLiteral("expires_at"))
                                    .toVariant().toLongLong();
        const QString workspace = result.body.value(QStringLiteral("wan_workspace_id"))
                                      .toString();
        static const QRegularExpression bailianKeyPattern(
            QStringLiteral("^st-[A-Za-z0-9._~+/-]{8,4093}={0,2}$"));
        static const QRegularExpression workspacePattern(
            QStringLiteral("^(?:ws|llm)-[A-Za-z0-9_-]{1,120}$"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (key.size() < 16 || key.size() > 4096 ||
            expiresAt <= now + 30 || expiresAt > now + 360 ||
            (bailian && (!bailianKeyPattern.match(key).hasMatch() ||
                         !workspacePattern.match(workspace).hasMatch()))) return;
        QMutexLocker lock(&modelCredentials_->mutex);
        if (bailian) {
            modelCredentials_->bailianKey = key;
            modelCredentials_->wanWorkspaceId = workspace;
            modelCredentials_->bailianExpiresAt = expiresAt;
        } else {
            modelCredentials_->arkKey = key;
            modelCredentials_->arkExpiresAt = expiresAt;
        }
    });
}

// 主动退出先清掉本地安全存储，再用进程内访问令牌尝试撤销服务端会话。
// 调用方在发起此请求前已经停止远程桌面并登出 IM，避免等待网络时继续收发消息。
void DesktopAuthClient::logout(Completion completion) {
    loggingOut_ = true;
    monitoringSession_ = false;
    monitoringModelCredentials_ = false;
    livenessTimer_.stop();
    modelCredentialTimer_.stop();
    credentialTimer_.stop();
    refreshTimer_.stop();
    clearModelCredentials();
    if (useSecureStore_) secureStore_.remove();
    auto finish = [this, completion = std::move(completion)](bool ok, const QString& error) {
        clearLocalSession();
        if (completion) completion(ok, error);
    };
    auto sendLogout = [this, finish] {
        request(QStringLiteral("auth/logout"), "POST", {}, accessToken_,
                [this, finish](const HttpResult& result) {
            finish(result.status == 200,
                   result.status == 200 ? QString() : responseError(result));
        });
    };
    if (refreshInFlight_ || accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([sendLogout, finish](bool ok, const QString& error) {
            if (ok) sendLogout();
            else finish(false, error);
        });
    } else {
        sendLogout();
    }
}

// 刷新令牌只能单飞：服务端轮换后重放旧令牌会撤销整条会话。
// 排队的完成回调在新令牌写入安全存储后统一唤醒。
void DesktopAuthClient::refresh(Completion completion) {
    if (completion) refreshCallbacks_.push_back(std::move(completion));
    if (refreshInFlight_) return;
    const QString token = refreshToken_.isEmpty() && useSecureStore_ ?
        secureStore_.read() : refreshToken_;
    if (!validToken(token)) {
        if (!userId_.isEmpty())
            invalidateSession(QStringLiteral("登录已失效，请重新扫码登录"));
        auto callbacks = std::move(refreshCallbacks_);
        refreshCallbacks_.clear();
        for (auto& callback : callbacks) callback(false, QStringLiteral("没有可恢复的登录"));
        return;
    }
    refreshInFlight_ = true;
    request(QStringLiteral("auth/refresh"), "POST",
            {{QStringLiteral("refresh_token"), token}}, {},
            [this](const HttpResult& result) {
        refreshInFlight_ = false;
        bool ok = false;
        QString error;
        if (result.status == 200) {
            const QString refreshedUserId = result.body.value(QStringLiteral("user"))
                                                .toObject().value(QStringLiteral("im_user_id"))
                                                .toString();
            if (!userId_.isEmpty() && userId_ != refreshedUserId) {
                error = QStringLiteral("账号身份校验失败，请重新扫码登录");
                invalidateSession(error);
            } else {
                ok = adoptSession(result.body, &error);
                if (!ok) invalidateSession(error);
            }
        } else {
            error = responseError(result);
            if (result.status == 401) invalidateSession(error);
        }
        auto callbacks = std::move(refreshCallbacks_);
        refreshCallbacks_.clear();
        for (auto& callback : callbacks) callback(ok, error);
        if (!ok && !refreshToken_.isEmpty()) refreshTimer_.start(10000);
    });
}

void DesktopAuthClient::startSessionMonitoring() {
    if (monitoringSession_ || userId_.isEmpty()) return;
    monitoringSession_ = true;
    if (qGuiApp && !foregroundHookInstalled_) {
        foregroundHookInstalled_ = true;
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) checkSessionNow();
        });
    }
    livenessTimer_.start();
    if (!qGuiApp || qGuiApp->applicationState() == Qt::ApplicationActive)
        checkSessionNow();
}

// 前台每 30 秒读一次 /auth/me；会话被同设备替换时旧访问令牌立即返回 401。
// 401 结果只针对发出请求时的令牌生效，避免与并发续期轮换发生误判。
void DesktopAuthClient::checkSessionNow() {
    if (!monitoringSession_ || livenessInFlight_ || userId_.isEmpty()) return;
    livenessInFlight_ = true;
    if (accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this](bool ok, const QString&) {
            livenessInFlight_ = false;
            if (ok) checkSessionNow();
        });
        return;
    }
    const QString token = accessToken_;
    const QString userId = userId_;
    request(QStringLiteral("auth/me"), "GET", {}, token,
            [this, token, userId](const HttpResult& result) {
        livenessInFlight_ = false;
        if (!monitoringSession_ || userId_ != userId || accessToken_ != token) return;
        if (result.status == 401) {
            invalidateSession(responseError(result));
            return;
        }
        if (result.status != 200) return;
        const QString checkedUserId = result.body.value(QStringLiteral("user"))
                                          .toObject().value(QStringLiteral("im_user_id"))
                                          .toString();
        if (checkedUserId != userId_ ||
            result.body.value(QStringLiteral("device_slot")).toString() != platformName()) {
            invalidateSession(QStringLiteral("账号身份校验失败，请重新扫码登录"));
            return;
        }
        nickname_ = result.body.value(QStringLiteral("user")).toObject()
                        .value(QStringLiteral("nickname")).toString();
    });
}

void DesktopAuthClient::restore(Completion completion) {
    if (refreshToken_.isEmpty() &&
        (!useSecureStore_ || secureStore_.read().isEmpty())) {
        completion(false, {});
        return;
    }
    refresh([this, completion = std::move(completion)](bool ok, const QString& error) {
        if (!ok) {
            refreshTimer_.stop();
            completion(false, error);
            return;
        }
        fetchImCredential([this, completion](bool credentialOk, const QString& credentialError) {
            if (!credentialOk) refreshTimer_.stop();
            else startModelCredentialMonitoring();
            completion(credentialOk, credentialError);
        });
    });
}

bool DesktopAuthClient::hasRestorableSession() const {
    return !refreshToken_.isEmpty() ||
           (useSecureStore_ && !secureStore_.read().isEmpty());
}

void DesktopAuthClient::prepareForQrLogin() {
    // 只有恢复已被确认不可继续时才调用；停止旧会话计时器，避免与新扫码轮换并行。
    if (!accessToken_.isEmpty() || !refreshToken_.isEmpty() || !userId_.isEmpty())
        clearLocalSession();
    refreshTimer_.stop();
    credentialTimer_.stop();
}

void DesktopAuthClient::createLoginRequest(
    std::function<void(const DesktopLoginRequest&, const QString&)> completion) {
    QByteArray raw(32, '\0');
    for (int i = 0; i < raw.size(); i += 4) {
        const quint32 word = QRandomGenerator::system()->generate();
        raw[i] = char(word);
        raw[i + 1] = char(word >> 8);
        raw[i + 2] = char(word >> 16);
        raw[i + 3] = char(word >> 24);
    }
    const QString verifier = QString::fromLatin1(base64Url(raw));
    const QString challenge = QString::fromLatin1(
        base64Url(QCryptographicHash::hash(raw, QCryptographicHash::Sha256)));
    request(QStringLiteral("auth/desktop/request"), "POST",
            {{QStringLiteral("platform"), platformName()},
             {QStringLiteral("code_challenge"), challenge}}, {},
            [this, verifier, completion = std::move(completion)](const HttpResult& result) {
        DesktopLoginRequest login;
        if (result.status != 200) {
            completion(login, responseError(result));
            return;
        }
        login.requestId = result.body.value(QStringLiteral("request_id")).toString();
        login.verifier = verifier;
        login.displayCode = result.body.value(QStringLiteral("display_code")).toString();
        login.expiresIn = result.body.value(QStringLiteral("expires_in")).toInt();
        login.qrPng = QByteArray::fromBase64(
            result.body.value(QStringLiteral("qr_png_base64")).toString().toLatin1());
        const QString expectedUri = QStringLiteral("maichat://auth/desktop?request_id=") +
                                    login.requestId;
        if (!validRequestId(login.requestId) ||
            result.body.value(QStringLiteral("qr_uri")).toString() != expectedUri ||
            !validDisplayCode(login.displayCode) || login.expiresIn <= 0 ||
            login.expiresIn > 120 || login.qrPng.isEmpty()) {
            completion({}, QStringLiteral("登录服务返回的二维码无效"));
            return;
        }
        completion(login, {});
    });
}

void DesktopAuthClient::claim(
    const QString& requestId, const QString& verifier,
    std::function<void(const ClaimResult&)> completion) {
    request(QStringLiteral("auth/desktop/claim"), "POST",
            {{QStringLiteral("request_id"), requestId},
             {QStringLiteral("verifier"), verifier}}, {},
            [this, completion = std::move(completion)](const HttpResult& result) {
        ClaimResult claimResult;
        if (result.status == 202) {
            claimResult.status = result.body.value(QStringLiteral("status")).toString() ==
                                         QStringLiteral("scanned") ?
                                     ClaimStatus::Scanned : ClaimStatus::Pending;
        } else if (result.status == 200) {
            claimResult.status = ClaimStatus::Approved;
            claimResult.session = result.body;
        } else if (result.status == 403) {
            claimResult.status =
                result.body.value(QStringLiteral("error")).toString() ==
                    QStringLiteral("Desktop login request denied") ?
                ClaimStatus::Denied : ClaimStatus::ApprovalRevoked;
        } else if (result.status == 410) {
            claimResult.status = ClaimStatus::Expired;
        } else {
            claimResult.error = responseError(result);
        }
        completion(claimResult);
    });
}

void DesktopAuthClient::cancelLoginRequest(const QString& requestId,
                                           const QString& verifier,
                                           std::function<void()> completion) {
    if (!validRequestId(requestId) || !validToken(verifier)) {
        if (completion) completion();
        return;
    }
    request(QStringLiteral("auth/desktop/cancel"), "POST",
            {{QStringLiteral("request_id"), requestId},
             {QStringLiteral("verifier"), verifier}}, {},
            [completion = std::move(completion)](const HttpResult&) {
        if (completion) completion();
    });
}

void DesktopAuthClient::revokeUnwantedClaim(const QJsonObject& session,
                                            std::function<void()> completion) {
    const QString access = session.value(QStringLiteral("access_token")).toString();
    if (!validToken(access) ||
        session.value(QStringLiteral("device_slot")).toString() != platformName()) {
        if (completion) completion();
        return;
    }
    // 旧 claim 可能在更换二维码后才签发会话；只撤销它，不改当前扫码会话或 Keychain。
    request(QStringLiteral("auth/logout"), "POST", {}, access,
            [completion = std::move(completion)](const HttpResult&) {
        if (completion) completion();
    });
}

void DesktopAuthClient::adoptApprovedSession(const QJsonObject& session,
                                             Completion completion) {
    QString error;
    if (!adoptSession(session, &error)) {
        completion(false, error);
        return;
    }
    fetchImCredential([this, completion = std::move(completion)](
                          bool ok, const QString& error) {
        if (ok) startModelCredentialMonitoring();
        completion(ok, error);
    });
}

void DesktopAuthClient::loginWithPassword(const QString& accountId,
                                          const QString& password, Completion completion) {
    const QString cleanAccount = accountId.trimmed();
    if (cleanAccount.isEmpty() || password.isEmpty()) {
        completion(false, QStringLiteral("请输入 MaiChat 账号和密码"));
        return;
    }
    QJsonObject body{{QStringLiteral("account_id"), cleanAccount},
                     {QStringLiteral("password"), password},
                     {QStringLiteral("age_confirmed_14_plus"), true},
                     {QStringLiteral("device_slot"), platformName()},
                     {QStringLiteral("device_platform"), platformName()}};
    request(QStringLiteral("auth/password/login"), "POST", body, {},
            [this, completion = std::move(completion)](const HttpResult& result) {
        if (result.status != 200) {
            completion(false, result.status == 401 ?
                QStringLiteral("账号或密码错误") : responseError(result));
            return;
        }
        adoptApprovedSession(result.body, completion);
    });
}

void DesktopAuthClient::updateNickname(const QString& nickname, Completion completion) {
    authorizedRequest(QStringLiteral("auth/profile"), "POST",
        {{QStringLiteral("nickname"), nickname}},
        [this, completion = std::move(completion)](const HttpResult& result) {
        if (result.status != 200) {
            if (completion) completion(false, responseError(result));
            return;
        }
        const QJsonObject user = result.body.value(QStringLiteral("user")).toObject();
        if (user.value(QStringLiteral("id")).toString() != accountId_ ||
            user.value(QStringLiteral("im_user_id")).toString() != userId_) {
            if (completion) completion(false, QStringLiteral("账号身份校验失败，请重新登录"));
            return;
        }
        nickname_ = user.value(QStringLiteral("nickname")).toString();
        if (completion) completion(true, {});
    });
}

void DesktopAuthClient::authorizedRequest(const QString& path, const QByteArray& method,
    const QJsonObject& body, std::function<void(const HttpResult&)> completion,
    bool retryAfterUnauthorized) {
    if (loggingOut_ || userId_.isEmpty()) {
        HttpResult failed;
        failed.error = QStringLiteral("请先登录 MaiChat");
        completion(failed);
        return;
    }
    if (accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this, path, method, body, completion, retryAfterUnauthorized](
                    bool ok, const QString& error) {
            if (ok) authorizedRequest(path, method, body, completion, retryAfterUnauthorized);
            else { HttpResult failed; failed.error = error; completion(failed); }
        });
        return;
    }
    request(path, method, body, accessToken_,
            [this, path, method, body, completion, retryAfterUnauthorized](const HttpResult& result) {
        if (result.status == 401 && retryAfterUnauthorized) {
            refresh([this, path, method, body, completion](bool ok, const QString& error) {
                if (ok) authorizedRequest(path, method, body, completion, false);
                else { HttpResult failed; failed.error = error; completion(failed); }
            });
            return;
        }
        if (result.status == 401) invalidateSession(responseError(result));
        completion(result);
    });
}

QUrl DesktopAuthClient::sessionSyncSocketUrl() const {
    QUrl url = baseUrl_.resolved(QUrl(QStringLiteral("session-sync/ws")));
    url.setScheme(url.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    return url;
}

std::function<QString()> DesktopAuthClient::sessionSyncAccessTokenProvider() const {
    const auto cache = accountAccess_;
    return [cache] {
        QMutexLocker lock(&cache->mutex);
        return cache->expiresAt > QDateTime::currentSecsSinceEpoch() + 30 ?
            cache->token : QString();
    };
}

void DesktopAuthClient::fetchFriends(FriendsCompletion completion) {
    authorizedRequest(QStringLiteral("friends"), "GET", {},
        [this, completion = std::move(completion)](const HttpResult& result) {
        if (result.status != 200 || !result.body.value(QStringLiteral("friends")).isArray()) {
            completion({}, responseError(result));
            return;
        }
        QList<DesktopFriend> friends;
        for (const QJsonValue& value : result.body.value(QStringLiteral("friends")).toArray()) {
            const QJsonObject row = value.toObject();
            DesktopFriend friendAccount{row.value(QStringLiteral("account_id")).toString(),
                row.value(QStringLiteral("nickname")).toString(),
                row.value(QStringLiteral("im_user_id")).toString()};
            if (friendAccount.accountId.isEmpty() || friendAccount.imUserId.isEmpty()) {
                completion({}, QStringLiteral("好友列表数据无效"));
                return;
            }
            friends.append(friendAccount);
        }
        completion(friends, {});
    });
}

void DesktopAuthClient::fetchFriendRequests(FriendRequestsCompletion completion) {
    authorizedRequest(QStringLiteral("friends/requests"), "GET", {},
        [this, completion = std::move(completion)](const HttpResult& result) {
        if (result.status != 200 || !result.body.value(QStringLiteral("incoming")).isArray()) {
            completion({}, responseError(result));
            return;
        }
        QList<DesktopFriendRequest> requests;
        for (const QJsonValue& value : result.body.value(QStringLiteral("incoming")).toArray()) {
            const QJsonObject row = value.toObject();
            DesktopFriendRequest request{row.value(QStringLiteral("id")).toString(),
                row.value(QStringLiteral("account_id")).toString(),
                row.value(QStringLiteral("nickname")).toString(),
                row.value(QStringLiteral("im_user_id")).toString()};
            if (!validRequestId(request.id) || request.accountId.isEmpty() ||
                request.imUserId.isEmpty()) {
                completion({}, QStringLiteral("好友申请数据无效"));
                return;
            }
            requests.append(request);
        }
        completion(requests, {});
    });
}

void DesktopAuthClient::requestFriend(const QString& account, Completion completion) {
    authorizedRequest(QStringLiteral("friends/requests"), "POST",
        QJsonObject{{QStringLiteral("account"), account.trimmed()}},
        [this, completion = std::move(completion)](const HttpResult& result) {
        completion(result.status == 200 || result.status == 201,
                   result.status == 200 || result.status == 201 ? QString() :
                   result.body.value(QStringLiteral("error")).toString(responseError(result)));
    });
}

void DesktopAuthClient::decideFriendRequest(const QString& requestId, bool accept,
                                            Completion completion) {
    if (!validRequestId(requestId)) {
        completion(false, QStringLiteral("好友申请无效"));
        return;
    }
    authorizedRequest(QStringLiteral("friends/requests/%1/%2").arg(
        requestId, accept ? QStringLiteral("accept") : QStringLiteral("reject")), "POST", {},
        [this, completion = std::move(completion)](const HttpResult& result) {
        completion(result.status == 200, result.status == 200 ? QString() :
                   result.body.value(QStringLiteral("error")).toString(responseError(result)));
    });
}

void DesktopAuthClient::removeFriend(const QString& account, Completion completion) {
    authorizedRequest(QStringLiteral("friends/remove"), "POST",
        QJsonObject{{QStringLiteral("account"), account}},
        [this, completion = std::move(completion)](const HttpResult& result) {
        completion(result.status == 200, result.status == 200 ? QString() :
                   result.body.value(QStringLiteral("error")).toString(responseError(result)));
    });
}

void DesktopAuthClient::fetchImCredential(Completion completion) {
    fetchImCredentialImpl(std::move(completion), true);
}

void DesktopAuthClient::fetchAccountModelKeys(AccountModelKeysCompletion completion) {
    fetchAccountModelKeysImpl(std::move(completion), true);
}

void DesktopAuthClient::fetchFriendQr(FriendQrCompletion completion) {
    fetchFriendQrImpl(std::move(completion), true);
}

void DesktopAuthClient::resolveFriendPhone(const QString& phone,
                                           FriendResolveCompletion completion) {
    resolveFriendPhoneImpl(phone, std::move(completion), true);
}

void DesktopAuthClient::resolveFriendPhoneImpl(const QString& phone,
                                               FriendResolveCompletion completion,
                                               bool retryAfterUnauthorized) {
    if (loggingOut_ || userId_.isEmpty()) {
        if (completion) completion({}, QStringLiteral("请先登录 MaiChat"));
        return;
    }
    if (accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this, phone, completion, retryAfterUnauthorized](bool ok, const QString& error) {
            if (ok) resolveFriendPhoneImpl(phone, completion, retryAfterUnauthorized);
            else if (completion) completion({}, error);
        });
        return;
    }
    request(QStringLiteral("friend/resolve"), "POST",
            QJsonObject{{QStringLiteral("phone"), phone}}, accessToken_,
            [this, phone, completion, retryAfterUnauthorized](const HttpResult& result) {
        if (loggingOut_ || userId_.isEmpty()) {
            if (completion) completion({}, QStringLiteral("本次登录已取消"));
            return;
        }
        if (result.status == 401 && retryAfterUnauthorized) {
            refresh([this, phone, completion](bool ok, const QString& error) {
                if (ok) resolveFriendPhoneImpl(phone, completion, false);
                else if (completion) completion({}, error);
            });
            return;
        }
        if (result.status != 200) {
            if (result.status == 401) invalidateSession(responseError(result));
            const QString error = result.status == 404 ?
                QStringLiteral("这个手机号尚未注册 MaiChat") :
                result.status == 429 ? QStringLiteral("查找好友过于频繁，请稍后再试") :
                result.status == 400 ? QStringLiteral("手机号无效或不能添加自己") :
                responseError(result);
            if (completion) completion({}, error);
            return;
        }
        const QString peerId = result.body.value(QStringLiteral("user_id")).toString();
        static const QRegularExpression accountId(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
        if (!accountId.match(peerId).hasMatch()) {
            if (completion) completion({}, QStringLiteral("好友账号资料无效"));
            return;
        }
        if (completion) completion(peerId, {});
    });
}

void DesktopAuthClient::fetchFriendQrImpl(FriendQrCompletion completion,
                                          bool retryAfterUnauthorized) {
    if (loggingOut_ || userId_.isEmpty()) {
        if (completion) completion({}, {}, QStringLiteral("请先登录 MaiChat"));
        return;
    }
    if (accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this, completion, retryAfterUnauthorized](bool ok, const QString& error) {
            if (ok) fetchFriendQrImpl(completion, retryAfterUnauthorized);
            else if (completion) completion({}, {}, error);
        });
        return;
    }
    request(QStringLiteral("friend/my-qr"), "GET", {}, accessToken_,
            [this, completion, retryAfterUnauthorized](const HttpResult& result) {
        if (loggingOut_ || userId_.isEmpty()) {
            if (completion) completion({}, {}, QStringLiteral("本次登录已取消"));
            return;
        }
        if (result.status == 401 && retryAfterUnauthorized) {
            refresh([this, completion](bool ok, const QString& error) {
                if (ok) fetchFriendQrImpl(completion, false);
                else if (completion) completion({}, {}, error);
            });
            return;
        }
        if (result.status != 200) {
            const QString error = responseError(result);
            if (result.status == 401) invalidateSession(error);
            if (completion) completion({}, {}, error);
            return;
        }
        const QString account = result.body.value(QStringLiteral("user_id")).toString();
        const QByteArray png = QByteArray::fromBase64(
            result.body.value(QStringLiteral("qr_png_base64")).toString().toLatin1());
        if (account != accountId_ || png.size() < 8 ||
            !png.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
            if (completion) completion({}, {}, QStringLiteral("好友二维码数据无效"));
            return;
        }
        if (completion) completion(account, png, {});
    });
}

void DesktopAuthClient::fetchAccountModelKeysImpl(AccountModelKeysCompletion completion,
                                               bool retryAfterUnauthorized) {
    if (loggingOut_ || userId_.isEmpty()) {
        if (completion) completion({}, QStringLiteral("请先登录 MaiChat"));
        return;
    }
    if (accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this, completion, retryAfterUnauthorized](bool ok, const QString& error) {
            if (ok) fetchAccountModelKeysImpl(completion, retryAfterUnauthorized);
            else if (completion) completion({}, error);
        });
        return;
    }
    request(QStringLiteral("model/api-keys"), "GET", {}, accessToken_,
            [this, completion, retryAfterUnauthorized](const HttpResult& result) {
        if (loggingOut_ || userId_.isEmpty()) {
            if (completion) completion({}, QStringLiteral("本次登录已取消"));
            return;
        }
        if (result.status == 401 && retryAfterUnauthorized) {
            refresh([this, completion](bool ok, const QString& error) {
                if (ok) fetchAccountModelKeysImpl(completion, false);
                else if (completion) completion({}, error);
            });
            return;
        }
        if (result.status != 200) {
            const QString error = result.status == 403 ?
                QStringLiteral("此账号尚未开通主聊天模型") : responseError(result);
            if (result.status == 401) invalidateSession(error);
            if (completion) completion({}, error);
            return;
        }
        const QJsonObject keys = result.body.value(QStringLiteral("api_keys")).toObject();
        if (!keys.value(QStringLiteral("glm")).isString() ||
            !keys.value(QStringLiteral("deepseek")).isString() ||
            (keys.contains(QStringLiteral("minimax")) &&
             !keys.value(QStringLiteral("minimax")).isString())) {
            if (completion)
                completion({}, QStringLiteral("账号服务返回的模型凭据无效"));
            return;
        }
        const QString glm = keys.value(QStringLiteral("glm")).toString();
        const QString deepseek = keys.value(QStringLiteral("deepseek")).toString();
        const QString minimax = keys.value(QStringLiteral("minimax")).toString();
        if ((!glm.isEmpty() && !validModelApiKey(glm)) ||
            (!deepseek.isEmpty() && !validModelApiKey(deepseek)) ||
            (!minimax.isEmpty() && !validModelApiKey(minimax)) ||
            (glm.isEmpty() && deepseek.isEmpty() && minimax.isEmpty())) {
            if (completion)
                completion({}, QStringLiteral("账号服务返回的模型凭据无效"));
            return;
        }
        QHash<QString, QString> available;
        if (!glm.isEmpty()) available.insert(QStringLiteral("glm"), glm);
        if (!deepseek.isEmpty()) available.insert(QStringLiteral("deepseek"), deepseek);
        if (!minimax.isEmpty()) available.insert(QStringLiteral("minimax"), minimax);
        if (completion) completion(available, {});
    });
}

// 取 IM 凭据前保证访问令牌有效；401 只刷新重试一次，防止无界循环。
// 成功更新缓存后通知调用方重新登录 SDK，否则旧 UserSig 仍可能被 SDK 持续使用。
void DesktopAuthClient::fetchImCredentialImpl(Completion completion,
                                             bool retryAfterUnauthorized) {
    if (loggingOut_) {
        if (completion) completion(false, QStringLiteral("本次登录已取消"));
        return;
    }
    if (accessToken_.isEmpty() ||
        accessExpiresAt_ <= QDateTime::currentDateTimeUtc().addSecs(60)) {
        refresh([this, completion = std::move(completion), retryAfterUnauthorized]
                (bool ok, const QString& error) {
            if (!ok) {
                if (completion) completion(false, error);
                return;
            }
            fetchImCredentialImpl(completion, retryAfterUnauthorized);
        });
        return;
    }
    request(QStringLiteral("im/credential"), "GET", {}, accessToken_,
            [this, completion = std::move(completion), retryAfterUnauthorized]
            (const HttpResult& result) {
        if (loggingOut_) {
            if (completion) completion(false, QStringLiteral("本次登录已取消"));
            return;
        }
        if (result.status == 401 && retryAfterUnauthorized) {
            refresh([this, completion](bool ok, const QString& error) {
                if (ok) fetchImCredentialImpl(completion, false);
                else if (completion) completion(false, error);
            });
            return;
        }
        if (result.status != 200) {
            if (result.status == 401) invalidateSession(responseError(result));
            else if (imCredential_.isValid()) credentialTimer_.start(60000);
            if (completion) completion(false, responseError(result));
            return;
        }
        DesktopImCredential credential;
        credential.sdkAppId = result.body.value(QStringLiteral("sdk_app_id")).toInt();
        credential.userId = result.body.value(QStringLiteral("user_id")).toString();
        credential.userSig = result.body.value(QStringLiteral("user_sig")).toString();
        credential.expiresAt = QDateTime::fromSecsSinceEpoch(
            result.body.value(QStringLiteral("expires_at")).toVariant().toLongLong(), Qt::UTC);
        if (credential.userId != userId_) {
            const QString error = QStringLiteral("IM 凭据与登录账号不一致");
            invalidateSession(error);
            if (completion) completion(false, error);
            return;
        }
        if (!credential.isValid()) {
            if (imCredential_.isValid()) credentialTimer_.start(60000);
            if (completion) completion(false, QStringLiteral("IM 凭据与登录账号不一致"));
            return;
        }
        if (imCredential_.isValid() && imCredential_.sdkAppId != credential.sdkAppId) {
            const QString error = QStringLiteral("IM 服务配置已变更，请重新扫码登录");
            invalidateSession(error);
            if (completion) completion(false, error);
            return;
        }
        const bool changed = imCredential_.userSig != credential.userSig ||
                             imCredential_.sdkAppId != credential.sdkAppId;
        imCredential_ = credential;
        if (changed && credentialUpdated_) credentialUpdated_(imCredential_);
        scheduleCredentialRefresh();
        if (completion) completion(true, {});
    });
}

void DesktopAuthClient::setCredentialUpdatedHandler(
    std::function<void(const DesktopImCredential&)> handler) {
    credentialUpdated_ = std::move(handler);
}

void DesktopAuthClient::setSessionInvalidatedHandler(
    std::function<void(const QString&)> handler) {
    sessionInvalidated_ = std::move(handler);
}
