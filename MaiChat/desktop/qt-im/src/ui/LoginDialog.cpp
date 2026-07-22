#include "ui/LoginDialog.h"

#include <QHBoxLayout>
#include <QLabel>
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
    setWindowTitle(QStringLiteral("远程 IM 登录"));
    setModal(true);
    setMinimumSize(860, 520);
    resize(920, 560);

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* introPane = new QWidget(this);
    introPane->setObjectName(QStringLiteral("introPane"));
    introPane->setMinimumWidth(330);
    auto* introLayout = new QVBoxLayout(introPane);
    introLayout->setContentsMargins(42, 44, 34, 44);
    introLayout->setSpacing(0);
    introLayout->addStretch(1);

    auto* title = makeTextLabel(QStringLiteral("远程 IM 登录"), QStringLiteral("loginTitle"), this);
    auto* subtitle = makeTextLabel(QStringLiteral("登录后再进入消息、通讯录和设置。"), QStringLiteral("loginSubtitle"), this);

    introLayout->addWidget(title);
    introLayout->addSpacing(10);
    introLayout->addWidget(subtitle);
    introLayout->addStretch(2);

    auto* formPane = new QWidget(this);
    formPane->setObjectName(QStringLiteral("formPane"));
    auto* formLayout = new QVBoxLayout(formPane);
    formLayout->setContentsMargins(48, 42, 48, 42);
    formLayout->setSpacing(0);
    formLayout->addStretch(1);

    auto* accountLabel = makeTextLabel(QStringLiteral("登录账号"), QStringLiteral("fieldLabel"), this);
    userIdInput_ = new QLineEdit(this);
    userIdInput_->setObjectName(QStringLiteral("userIdInput"));
    userIdInput_->setPlaceholderText(QStringLiteral("输入 IM 账号 ID"));
    userIdInput_->setClearButtonEnabled(true);
    formLayout->addWidget(accountLabel);
    formLayout->addSpacing(9);
    formLayout->addWidget(userIdInput_);
    formLayout->addSpacing(26);

    loginButton_ = new QPushButton(QStringLiteral("登录并进入"), this);
    loginButton_->setObjectName(QStringLiteral("loginButton"));
    loginButton_->setCursor(Qt::PointingHandCursor);
    formLayout->addWidget(loginButton_);

    formLayout->addStretch(1);

    rootLayout->addWidget(introPane, 3);
    rootLayout->addWidget(formPane, 4);

    userIdInput_->setText(envValue(QStringLiteral("MAICHAT_USER_ID")));

    connect(userIdInput_, &QLineEdit::textChanged, this, [this] { updateLoginButton(); });
    connect(loginButton_, &QPushButton::clicked, this, [this] {
        if (!userId().isEmpty()) accept();
    });
}

void LoginDialog::applyStyle() {
    setStyleSheet(QStringLiteral(R"(
        LoginDialog {
            background: #f6f9fc;
        }
        #introPane {
            background: #f6f9fc;
            border-right: 1px solid #d8e2f0;
        }
        #formPane {
            background: #ffffff;
        }
        #loginTitle {
            color: #0e1628;
            font-size: 32px;
            font-weight: 800;
        }
        #loginSubtitle {
            color: #66758c;
            font-size: 15px;
            font-weight: 700;
        }
        #fieldLabel, #credentialHeader {
            color: #66758c;
            font-size: 15px;
            font-weight: 800;
        }
        #userIdInput {
            min-height: 52px;
            border: 1px solid #d8e2f0;
            border-radius: 10px;
            background: #ffffff;
            color: #101828;
            padding: 0 14px;
            font-size: 17px;
        }
        #userIdInput:focus {
            border-color: #9ad7ff;
            background: #ffffff;
        }
        #loginButton {
            min-height: 56px;
            border: 0;
            border-radius: 11px;
            color: #ffffff;
            font-size: 17px;
            font-weight: 800;
        }
    )"));
}

void LoginDialog::updateLoginButton() {
    const bool canSubmit = !userId().isEmpty();
    loginButton_->setEnabled(canSubmit);
    loginButton_->setStyleSheet(canSubmit
                                    ? QStringLiteral("#loginButton { background: #0f8dde; color: #ffffff; } #loginButton:hover { background: #087fc9; }")
                                    : QStringLiteral("#loginButton { background: #acd2ea; color: #e8f4fb; }"));
}
