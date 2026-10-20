// 文件作用：桌面端登录窗口，在扫码和账号密码登录之间切换。
// 类型关系：LoginDialog 继承 QDialog，使用 DesktopAuthClient 执行网络请求和凭据获取。
// 对外边界：切换或关闭扫码页时废弃旧 verifier，不接受过期的异步登录结果。
// 直接依赖：Qt Widgets、DesktopAuthClient 和现有 MaiChat 视觉资源。
#include "ui/LoginDialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
QLabel* makeLabel(const QString& text, const QString& name, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(name);
    label->setAlignment(Qt::AlignCenter);
    return label;
}
}

LoginDialog::LoginDialog(DesktopAuthClient* auth, QWidget* parent)
    : QDialog(parent), auth_(auth) {
    Q_INIT_RESOURCE(resources);
    buildUi();
    applyStyle();
    setState(State::Loading);
    countdownTimer_.setInterval(1000);
    connect(&countdownTimer_, &QTimer::timeout, this, &LoginDialog::updateCountdown);
    pollTimer_.setSingleShot(true);
    connect(&pollTimer_, &QTimer::timeout, this, &LoginDialog::pollClaim);
}

// 两种登录表单留在同一面板中，切换时只改变可见性，不重建已输入的内容。
void LoginDialog::buildUi() {
    setWindowTitle(QStringLiteral("MaiChat"));
    setModal(true);
    setMinimumSize(480, 550);
    resize(520, 610);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    auto* modeRow = new QHBoxLayout;
    modeRow->addStretch();
    modeButton_ = new QPushButton(QStringLiteral("账号密码登录"), this);
    modeButton_->setObjectName(QStringLiteral("loginModeButton"));
    modeButton_->setCursor(Qt::PointingHandCursor);
    modeRow->addWidget(modeButton_);
    root->addLayout(modeRow);
    root->addStretch();
    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("loginPanel"));
    panel->setFixedWidth(400);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* logo = makeLabel({}, QStringLiteral("loginLogo"), panel);
    logo->setFixedSize(48, 48);
    const QPixmap icon(QStringLiteral(":/maichat/app-icon.png"));
    const qreal dpr = devicePixelRatioF();
    QPixmap logoPixmap = icon.scaled(QSize(48, 48) * dpr, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
    logoPixmap.setDevicePixelRatio(dpr);
    logo->setPixmap(logoPixmap);
    layout->addWidget(logo, 0, Qt::AlignHCenter);
    layout->addSpacing(8);
    layout->addWidget(makeLabel(QStringLiteral("MaiChat"),
                                QStringLiteral("welcomeTitle"), panel));
    layout->addSpacing(4);
    subtitleLabel_ = makeLabel(QStringLiteral("使用手机扫码登录"),
                               QStringLiteral("loginSubtitle"), panel);
    layout->addWidget(subtitleLabel_);
    layout->addSpacing(14);

    qrPane_ = new QWidget(panel);
    qrPane_->setObjectName(QStringLiteral("loginQrPane"));
    auto* qrPaneLayout = new QVBoxLayout(qrPane_);
    qrPaneLayout->setContentsMargins(0, 0, 0, 0);
    qrPaneLayout->setSpacing(0);
    auto* frame = new QWidget(qrPane_);
    frame->setObjectName(QStringLiteral("qrFrame"));
    frame->setFixedSize(220, 220);
    auto* qrLayout = new QVBoxLayout(frame);
    qrLayout->setContentsMargins(14, 14, 14, 14);
    qrLabel_ = makeLabel(QStringLiteral("正在生成二维码…"),
                         QStringLiteral("loginQr"), frame);
    qrLabel_->setFixedSize(192, 192);
    qrLayout->addWidget(qrLabel_);
    qrPaneLayout->addWidget(frame, 0, Qt::AlignHCenter);
    qrPaneLayout->addSpacing(10);
    codeLabel_ = makeLabel({}, QStringLiteral("loginCode"), qrPane_);
    qrPaneLayout->addWidget(codeLabel_);
    qrPaneLayout->addSpacing(5);
    statusLabel_ = makeLabel({}, QStringLiteral("loginStatus"), qrPane_);
    qrPaneLayout->addWidget(statusLabel_);
    qrPaneLayout->addSpacing(4);
    detailLabel_ = makeLabel({}, QStringLiteral("loginDetail"), qrPane_);
    detailLabel_->setWordWrap(true);
    qrPaneLayout->addWidget(detailLabel_);
    countdownLabel_ = makeLabel({}, QStringLiteral("loginCountdown"), qrPane_);
    qrPaneLayout->addWidget(countdownLabel_);
    qrPaneLayout->addSpacing(10);
    retryButton_ = new QPushButton(QStringLiteral("更换二维码"), qrPane_);
    retryButton_->setObjectName(QStringLiteral("loginRetryButton"));
    retryButton_->setFixedHeight(42);
    retryButton_->setCursor(Qt::PointingHandCursor);
    qrPaneLayout->addWidget(retryButton_);
    layout->addWidget(qrPane_);

    passwordPane_ = new QWidget(panel);
    passwordPane_->setObjectName(QStringLiteral("loginPasswordPane"));
    auto* passwordLayout = new QVBoxLayout(passwordPane_);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->setSpacing(0);
    accountInput_ = new QLineEdit(passwordPane_);
    accountInput_->setObjectName(QStringLiteral("accountLoginId"));
    accountInput_->setPlaceholderText(QStringLiteral("MaiChat 账号 ID"));
    accountInput_->setMaxLength(36);
    passwordLayout->addWidget(accountInput_);
    passwordLayout->addSpacing(7);
    passwordInput_ = new QLineEdit(passwordPane_);
    passwordInput_->setObjectName(QStringLiteral("accountLoginPassword"));
    passwordInput_->setPlaceholderText(QStringLiteral("密码"));
    passwordInput_->setEchoMode(QLineEdit::Password);
    passwordLayout->addWidget(passwordInput_);
    passwordLayout->addSpacing(8);
    passwordButton_ = new QPushButton(QStringLiteral("账号密码登录"), passwordPane_);
    passwordButton_->setObjectName(QStringLiteral("accountLoginButton"));
    passwordButton_->setFixedHeight(42);
    passwordLayout->addWidget(passwordButton_);
    passwordErrorLabel_ = makeLabel({}, QStringLiteral("accountLoginError"), passwordPane_);
    passwordErrorLabel_->setWordWrap(true);
    passwordLayout->addWidget(passwordErrorLabel_);
    passwordErrorLabel_->hide();
    passwordLayout->addSpacing(12);
    agreementCheck_ = new QCheckBox(QStringLiteral("我已年满 14 周岁，并同意下方协议"), passwordPane_);
    agreementCheck_->setObjectName(QStringLiteral("accountLoginAgreement"));
    passwordLayout->addWidget(agreementCheck_);
    auto* legal = makeLabel(QStringLiteral(
        "<a href='https://ichat.life/legal/terms.html'>用户协议</a> · "
        "<a href='https://ichat.life/legal/privacy.html'>隐私政策</a>"),
        QStringLiteral("accountLoginLegal"), passwordPane_);
    legal->setOpenExternalLinks(true);
    passwordLayout->addWidget(legal);
    layout->addWidget(passwordPane_);
    passwordPane_->hide();
    layout->addSpacing(14);
    auto* cancelButton = new QPushButton(QStringLiteral("取消"), panel);
    cancelButton->setObjectName(QStringLiteral("loginCancelButton"));
    cancelButton->setFixedHeight(30);
    cancelButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(cancelButton);
    root->addWidget(panel, 0, Qt::AlignHCenter);
    root->addStretch();
    connect(modeButton_, &QPushButton::clicked, this, &LoginDialog::toggleLoginMode);
    connect(retryButton_, &QPushButton::clicked, this, &LoginDialog::retry);
    connect(passwordButton_, &QPushButton::clicked, this, &LoginDialog::loginWithPassword);
    connect(passwordInput_, &QLineEdit::returnPressed, this, &LoginDialog::loginWithPassword);
    connect(cancelButton, &QPushButton::clicked, this, &LoginDialog::reject);
}

void LoginDialog::applyStyle() {
    setStyleSheet(QStringLiteral(R"(
        LoginDialog { background: #ffffff; }
        #loginPanel, #loginLogo, #loginQrPane, #loginPasswordPane { background: transparent; }
        #loginModeButton { background: transparent; border: 0; color: #0871ba;
            font-size: 13px; padding: 6px 8px; }
        #loginModeButton:hover { color: #005b9a; }
        #welcomeTitle { color: #0f172a; font-size: 20px; font-weight: 700; }
        #loginSubtitle { color: #64748b; font-size: 13px; }
        #qrFrame { background: #ffffff; border: 1px solid #e2e8f0; border-radius: 16px; }
        #loginQr { color: #64748b; font-size: 14px; border: 0; }
        #loginCode { color: #334155; font-size: 15px; font-weight: 600; }
        #loginStatus { color: #0f172a; font-size: 15px; font-weight: 600; }
        #loginDetail, #loginCountdown { color: #64748b; font-size: 12px; }
        #accountLoginError { color: #d64242; font-size: 12px; }
        #accountLoginAgreement, #accountLoginLegal { color: #64748b; font-size: 12px; }
        #accountLoginId, #accountLoginPassword { border: 1px solid #d9e2ed;
            border-radius: 9px; padding: 8px 12px; min-height: 24px; background: #ffffff; }
        #accountLoginButton { background: #e8f3ff; color: #0871ba; border: 0;
            border-radius: 9px; font-size: 14px; font-weight: 600; }
        #loginRetryButton { border: 0; border-radius: 10px; background: #2f81f7;
                            color: #ffffff; font-size: 14px; font-weight: 700; }
        #loginRetryButton:hover { background: #256fe0; }
        #loginRetryButton:pressed { background: #1f63cf; }
        #loginRetryButton:disabled { background: #eef1f5; color: #aab4c3; }
        #loginCancelButton { border: 0; background: transparent; color: #64748b;
                             font-size: 13px; }
        #loginCancelButton:hover { color: #0f172a; }
    )"));
}

// 所有可见状态集中处理：每次状态转换同时更新文案、按钮、二维码和倒计时，
// 防止过期或拒绝后旧码仍可见、连接中仍能重复提交。
void LoginDialog::setState(State state, const QString& detail) {
    state_ = state;
    QString status, hint = detail, action = QStringLiteral("更换二维码");
    bool enabled = true;
    const bool showQr = !request_.qrPng.isEmpty() && !request_.verifier.isEmpty() &&
        (state == State::Waiting || state == State::Scanned ||
         state == State::NetworkError);
    switch (state) {
    case State::Restoring:
        status = QStringLiteral("正在恢复登录");
        hint.clear();
        enabled = false;
        break;
    case State::RestoreError:
        status = QStringLiteral("恢复登录失败");
        if (hint.isEmpty()) hint = QStringLiteral("检查网络后重试");
        action = QStringLiteral("重试恢复登录");
        break;
    case State::Loading:
        status = QStringLiteral("正在生成二维码");
        hint.clear();
        enabled = false;
        break;
    case State::Waiting:
        status = QStringLiteral("等待手机扫码并确认");
        hint.clear();
        break;
    case State::Scanned:
        status = QStringLiteral("已扫码，请在手机上确认");
        hint.clear();
        break;
    case State::Expired:
        status = QStringLiteral("二维码已过期");
        hint.clear();
        action = QStringLiteral("重新生成");
        break;
    case State::Denied:
        status = QStringLiteral("手机拒绝了这次登录");
        hint.clear();
        action = QStringLiteral("重新生成");
        break;
    case State::ApprovalRevoked:
        status = QStringLiteral("手机授权已失效");
        hint.clear();
        action = QStringLiteral("重新生成");
        break;
    case State::NetworkError:
        status = QStringLiteral("网络连接失败");
        if (hint.isEmpty()) hint = QStringLiteral("检查网络后重试");
        action = QStringLiteral("重试连接");
        break;
    case State::Connecting:
        status = QStringLiteral("登录成功，正在连接");
        hint.clear();
        enabled = false;
        break;
    case State::CredentialError:
        status = QStringLiteral("连接聊天服务失败");
        action = auth_ && auth_->authenticatedUserId().isEmpty() ?
            QStringLiteral("生成新二维码") : QStringLiteral("重试连接");
        break;
    }
    statusLabel_->setText(status);
    statusLabel_->setVisible(showQr);
    detailLabel_->setText(hint);
    detailLabel_->setVisible(!hint.isEmpty());
    retryButton_->setText(action);
    retryButton_->setEnabled(enabled);
    modeButton_->setEnabled(state != State::Restoring && state != State::Connecting &&
                            !passwordInFlight_);
    if (showQr) {
        QPixmap pixmap;
        pixmap.loadFromData(request_.qrPng, "PNG");
        qrLabel_->setPixmap(pixmap.width() <= 192 && pixmap.height() <= 192 ?
            pixmap : pixmap.scaled(192, 192, Qt::KeepAspectRatio,
                                   Qt::FastTransformation));
    } else {
        qrLabel_->setPixmap({});
        qrLabel_->setText(status);
    }
    codeLabel_->setText(showQr ? QStringLiteral("核对码  %1").arg(request_.displayCode) : QString());
    countdownLabel_->setVisible(showQr && !expiresAt_.isNull());
}

void LoginDialog::start() {
    if (!auth_) return;
    setState(State::Restoring);
    const quint64 generation = ++generation_;
    auth_->restore([this, generation](bool ok, const QString& error) {
        if (generation != generation_) return;
        if (ok) accept();
        else if (auth_->hasRestorableSession()) setState(State::RestoreError, error);
        else requestQr();
    });
}

void LoginDialog::toggleLoginMode() {
    if (closing_ || passwordInFlight_ || state_ == State::Restoring ||
        state_ == State::Connecting) return;
    passwordMode_ = !passwordMode_;
    qrPane_->setVisible(!passwordMode_);
    passwordPane_->setVisible(passwordMode_);
    subtitleLabel_->setText(passwordMode_ ? QStringLiteral("使用 MaiChat 账号密码登录") :
                                            QStringLiteral("使用手机扫码登录"));
    modeButton_->setText(passwordMode_ ? QStringLiteral("扫码登录") :
                                        QStringLiteral("账号密码登录"));
    passwordErrorLabel_->clear();
    passwordErrorLabel_->hide();
    if (!passwordMode_) {
        requestQr();
        return;
    }
    ++generation_;
    qrRestartPending_ = false;
    pollInFlight_ = false;
    countdownTimer_.stop();
    pollTimer_.stop();
    const DesktopLoginRequest previous = request_;
    request_ = {};
    expiresAt_ = {};
    cancelRequest(previous);
    accountInput_->setFocus(Qt::OtherFocusReason);
}

void LoginDialog::loginWithPassword() {
    if (!auth_ || closing_ || passwordInFlight_ || !passwordMode_) return;
    const QString accountId = accountInput_->text().trimmed();
    const QString password = passwordInput_->text();
    if (accountId.isEmpty() || password.isEmpty()) {
        passwordErrorLabel_->setText(QStringLiteral("请输入 MaiChat 账号和密码"));
        passwordErrorLabel_->show();
        return;
    }
    if (!agreementCheck_->isChecked()) {
        passwordErrorLabel_->setText(QStringLiteral("请先确认年龄并同意协议"));
        passwordErrorLabel_->show();
        return;
    }
    passwordInFlight_ = true;
    passwordButton_->setEnabled(false);
    passwordButton_->setText(QStringLiteral("正在登录…"));
    modeButton_->setEnabled(false);
    passwordErrorLabel_->hide();
    const quint64 generation = ++generation_;
    const DesktopLoginRequest previous = request_;
    request_ = {};
    countdownTimer_.stop();
    pollTimer_.stop();
    cancelRequest(previous);
    QPointer<LoginDialog> self(this);
    DesktopAuthClient* auth = auth_;
    auth_->loginWithPassword(accountId, password,
        [self, auth, generation](bool ok, const QString& error) {
        if (!self) {
            if (ok) auth->logout({});
            return;
        }
        self->passwordInFlight_ = false;
        self->passwordInput_->clear();
        self->passwordButton_->setEnabled(true);
        self->passwordButton_->setText(QStringLiteral("账号密码登录"));
        self->modeButton_->setEnabled(true);
        if (self->closing_ || generation != self->generation_) {
            if (ok) auth->logout({});
            return;
        }
        if (ok) self->accept();
        else {
            self->passwordErrorLabel_->setText(error.isEmpty() ?
                QStringLiteral("登录失败，请重试") : error);
            self->passwordErrorLabel_->show();
        }
    });
}

void LoginDialog::requestQr() {
    if (!auth_) return;
    auth_->prepareForQrLogin();
    ++generation_;
    const DesktopLoginRequest previous = request_;
    request_ = {};
    expiresAt_ = {};
    pollInFlight_ = false;
    countdownTimer_.stop();
    pollTimer_.stop();
    cancelRequest(previous);
    setState(State::Loading);
    qrRestartPending_ = true;
    continueQrRequest();
}

void LoginDialog::continueQrRequest() {
    if (!qrRestartPending_ || closing_ || pendingClaimCount_ != 0 ||
        pendingCancellationCount_ != 0 || pendingRevocationCount_ != 0) return;
    qrRestartPending_ = false;
    const quint64 generation = generation_;
    auth_->createLoginRequest([this, generation](const DesktopLoginRequest& request,
                                                  const QString& error) {
        if (generation != generation_) return;
        if (!error.isEmpty()) {
            setState(State::NetworkError, error);
            return;
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(request.qrPng, "PNG")) {
            setState(State::NetworkError, QStringLiteral("登录二维码图片无效"));
            return;
        }
        request_ = request;
        expiresAt_ = QDateTime::currentDateTimeUtc().addSecs(request.expiresIn);
        setState(State::Waiting);
        updateCountdown();
        countdownTimer_.start();
        pollTimer_.start(2000);
    });
}

void LoginDialog::updateCountdown() {
    const qint64 remaining = QDateTime::currentDateTimeUtc().secsTo(expiresAt_);
    if (remaining <= 0) {
        countdownTimer_.stop();
        pollTimer_.stop();
        request_.verifier.clear();
        setState(State::Expired);
        return;
    }
    countdownLabel_->setText(QStringLiteral("%1 秒后过期").arg(remaining));
}

// 同一时刻只发送一条 claim。generation 保证更换二维码或取消登录后，
// 旧网络回复既不能改新页面，也不能接受已经被废弃的会话。
void LoginDialog::pollClaim() {
    if (!auth_ || pollInFlight_ || request_.verifier.isEmpty()) return;
    pollInFlight_ = true;
    ++pendingClaimCount_;
    const quint64 generation = generation_;
    auth_->claim(request_.requestId, request_.verifier,
                 [this, generation](const DesktopAuthClient::ClaimResult& result) {
        --pendingClaimCount_;
        if (generation != generation_ || closing_) {
            if (result.status == DesktopAuthClient::ClaimStatus::Approved)
                revokeUnwantedClaim(result.session);
            continueQrRequest();
            finishCancellation();
            return;
        }
        pollInFlight_ = false;
        if (state_ == State::Expired &&
            result.status != DesktopAuthClient::ClaimStatus::Approved) return;
        switch (result.status) {
        case DesktopAuthClient::ClaimStatus::Pending:
            setState(State::Waiting);
            pollTimer_.start(2000);
            break;
        case DesktopAuthClient::ClaimStatus::Scanned:
            setState(State::Scanned);
            pollTimer_.start(2000);
            break;
        case DesktopAuthClient::ClaimStatus::Denied:
            countdownTimer_.stop();
            request_.verifier.clear();
            setState(State::Denied);
            break;
        case DesktopAuthClient::ClaimStatus::ApprovalRevoked:
            countdownTimer_.stop();
            request_.verifier.clear();
            setState(State::ApprovalRevoked);
            break;
        case DesktopAuthClient::ClaimStatus::Expired:
            countdownTimer_.stop();
            request_.verifier.clear();
            setState(State::Expired);
            break;
        case DesktopAuthClient::ClaimStatus::Approved:
            countdownTimer_.stop();
            request_.verifier.clear();
            setState(State::Connecting);
            auth_->adoptApprovedSession(result.session,
                [this, generation](bool ok, const QString& error) {
                    if (generation != generation_) return;
                    if (ok) accept();
                    else {
                        if (auth_->authenticatedUserId().isEmpty()) {
                            request_ = {};
                            expiresAt_ = {};
                        }
                        setState(State::CredentialError, error);
                    }
                });
            break;
        case DesktopAuthClient::ClaimStatus::Failed:
            setState(State::NetworkError, result.error);
            pollTimer_.start(5000);
            break;
        }
    });
}

void LoginDialog::retry() {
    if (!auth_) return;
    if (state_ == State::RestoreError) {
        start();
    } else if (state_ == State::CredentialError) {
        if (auth_->authenticatedUserId().isEmpty()) {
            requestQr();
            return;
        }
        setState(State::Connecting);
        const quint64 generation = generation_;
        auth_->fetchImCredential([this, generation](bool ok, const QString& error) {
            if (generation != generation_) return;
            if (ok) accept();
            else {
                if (auth_->authenticatedUserId().isEmpty()) {
                    request_ = {};
                    expiresAt_ = {};
                }
                setState(State::CredentialError, error);
            }
        });
    } else if (state_ == State::NetworkError && !request_.verifier.isEmpty()) {
        setState(State::Waiting);
        pollTimer_.stop();
        pollClaim();
    } else {
        requestQr();
    }
}

void LoginDialog::reject() {
    if (closing_) return;
    closing_ = true;
    cancellationSetupComplete_ = false;
    qrRestartPending_ = false;
    ++generation_;
    countdownTimer_.stop();
    pollTimer_.stop();
    const DesktopLoginRequest previous = request_;
    request_ = {};
    // 先隐藏，再等待在途 claim 与撤销结果，避免 claim200 已签发会话却丢掉令牌。
    hide();
    cancelRequest(previous);
    if (auth_ && !auth_->authenticatedUserId().isEmpty()) {
        ++pendingLogoutCount_;
        auth_->logout([this](bool, const QString&) {
            --pendingLogoutCount_;
            finishCancellation();
        });
    }
    cancellationSetupComplete_ = true;
    finishCancellation();
}

void LoginDialog::finishCancellation() {
    if (!closing_ || !cancellationSetupComplete_) return;
    if (pendingClaimCount_ != 0 || pendingCancellationCount_ != 0 ||
        pendingRevocationCount_ != 0 || pendingLogoutCount_ != 0) return;
    closing_ = false;
    QDialog::reject();
}

void LoginDialog::cancelRequest(const DesktopLoginRequest& request) {
    if (!auth_ || request.verifier.isEmpty()) return;
    ++pendingCancellationCount_;
    auth_->cancelLoginRequest(request.requestId, request.verifier, [this] {
        --pendingCancellationCount_;
        continueQrRequest();
        finishCancellation();
    });
}

void LoginDialog::revokeUnwantedClaim(const QJsonObject& session) {
    if (!auth_) return;
    ++pendingRevocationCount_;
    auth_->revokeUnwantedClaim(session, [this] {
        --pendingRevocationCount_;
        continueQrRequest();
        finishCancellation();
    });
}
