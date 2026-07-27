#include "ui/LoginDialog.h"

#include <QLabel>
#include <QPixmap>
#include <QProcessEnvironment>
#include <QVBoxLayout>

namespace {

QLabel* makeTextLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

QString envValue(const QString& name) {
    return QProcessEnvironment::systemEnvironment().value(name).trimmed();
}

}  // namespace

LoginDialog::LoginDialog(QWidget* parent) : QDialog(parent) {
    Q_INIT_RESOURCE(resources);
    buildUi();
    applyStyle();
    updateLoginButton();
}

QString LoginDialog::userId() const {
    return userIdInput_->text().trimmed();
}

void LoginDialog::setUserId(const QString& userId) {
    userIdInput_->setText(userId.trimmed());
    updateLoginButton();
}

void LoginDialog::buildUi() {
    setWindowTitle(QStringLiteral("MaiChat"));
    setModal(true);
    setMinimumSize(720, 480);
    resize(920, 560);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(32, 32, 32, 32);
    rootLayout->setSpacing(0);
    rootLayout->addStretch(1);

    auto* loginPanel = new QWidget(this);
    loginPanel->setObjectName(QStringLiteral("loginPanel"));
    loginPanel->setFixedWidth(420);
    auto* loginLayout = new QVBoxLayout(loginPanel);
    loginLayout->setContentsMargins(0, 0, 0, 0);
    loginLayout->setSpacing(0);

    auto* logo = makeTextLabel(QString(), QStringLiteral("loginLogo"), loginPanel);
    logo->setAlignment(Qt::AlignCenter);
    logo->setFixedSize(64, 64);
    const QPixmap appIcon(QStringLiteral(":/maichat/app-icon.png"));
    // 高分屏（DPR>1）按物理分辨率缩放并声明 DPR，否则 64 逻辑像素的位图会被
    // 绘制层再放大一次，logo 发虚（源图 1024px，分辨率充足）。
    const qreal logoDpr = devicePixelRatioF();
    QPixmap logoPixmap = appIcon.scaled(QSize(64, 64) * logoDpr,
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    logoPixmap.setDevicePixelRatio(logoDpr);
    logo->setPixmap(logoPixmap);

    auto* title = makeTextLabel(QStringLiteral("欢迎使用 MaiChat"), QStringLiteral("welcomeTitle"), loginPanel);
    title->setAlignment(Qt::AlignCenter);

    loginLayout->addWidget(logo, 0, Qt::AlignHCenter);
    loginLayout->addSpacing(12);
    loginLayout->addWidget(title);
    loginLayout->addSpacing(28);

    userIdInput_ = new QLineEdit(loginPanel);
    userIdInput_->setObjectName(QStringLiteral("userIdInput"));
    userIdInput_->setPlaceholderText(QStringLiteral("请输入登录账号"));
    userIdInput_->setClearButtonEnabled(true);
    userIdInput_->setFixedHeight(46);
    loginLayout->addWidget(userIdInput_);
    loginLayout->addSpacing(16);

    loginButton_ = new QPushButton(QStringLiteral("登录"), loginPanel);
    loginButton_->setObjectName(QStringLiteral("loginButton"));
    loginButton_->setCursor(Qt::PointingHandCursor);
    loginButton_->setDefault(true);
    loginButton_->setFixedHeight(46);
    loginLayout->addWidget(loginButton_);

    rootLayout->addWidget(loginPanel, 0, Qt::AlignHCenter);
    rootLayout->addStretch(1);

    userIdInput_->setText(envValue(QStringLiteral("MAICHAT_USER_ID")));

    connect(userIdInput_, &QLineEdit::textChanged, this, [this] { updateLoginButton(); });
    connect(loginButton_, &QPushButton::clicked, this, [this] {
        if (!userId().isEmpty()) accept();
    });
}

void LoginDialog::applyStyle() {
    setStyleSheet(QStringLiteral(R"(
        LoginDialog {
            background: #ffffff;
        }
        #loginPanel {
            background: transparent;
        }
        #loginLogo {
            background: transparent;
        }
        #welcomeTitle {
            color: #0f172a;
            font-size: 20px;
            font-weight: 800;
        }
        #userIdInput {
            border: 1px solid #d9e1ec;
            border-radius: 10px;
            background: #ffffff;
            color: #0f172a;
            padding: 0 14px;
            font-size: 14px;
            selection-background-color: #2f81f7;
        }
        #userIdInput:focus {
            border: 2px solid #2f81f7;
        }
        #loginButton {
            border: 0;
            border-radius: 10px;
            background: #2f81f7;
            color: #ffffff;
            font-size: 15px;
            font-weight: 800;
        }
        #loginButton:hover {
            background: #256fe0;
        }
        #loginButton:pressed {
            background: #1f63cf;
        }
        #loginButton:disabled {
            background: #eef1f5;
            color: #aab4c3;
        }
    )"));
}

void LoginDialog::updateLoginButton() {
    loginButton_->setEnabled(!userId().isEmpty());
}
