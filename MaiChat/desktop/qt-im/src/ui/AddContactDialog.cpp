#include "ui/AddContactDialog.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace {

QLabel* makeLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

}  // namespace

AddContactDialog::AddContactDialog(QWidget* parent) : QDialog(parent) {
    buildUi();
    applyStyle();
    updateConfirmButton();
}

QString AddContactDialog::userId() const {
    return userIdInput_->text().trimmed();
}

void AddContactDialog::setUserId(const QString& userId) {
    userIdInput_->setText(userId.trimmed());
    updateConfirmButton();
}

void AddContactDialog::buildUi() {
    setObjectName(QStringLiteral("addContactDialog"));
    setWindowTitle(QStringLiteral("添加联系人"));
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(520, 320);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("addContactPanel"));
    auto* shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(32);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(16, 24, 40, 38));
    panel->setGraphicsEffect(shadow);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(26, 24, 26, 22);
    panelLayout->setSpacing(0);

    auto* title = makeLabel(QStringLiteral("添加联系人"), QStringLiteral("addContactTitle"), panel);
    auto* subtitle = makeLabel(QStringLiteral("输入对方 IM 账号 ID，添加后会自动打开会话。"), QStringLiteral("addContactSubtitle"), panel);
    panelLayout->addWidget(title);
    panelLayout->addSpacing(8);
    panelLayout->addWidget(subtitle);
    panelLayout->addSpacing(22);

    auto* fieldLabel = makeLabel(QStringLiteral("账号 ID"), QStringLiteral("addContactFieldLabel"), panel);
    userIdInput_ = new QLineEdit(panel);
    userIdInput_->setObjectName(QStringLiteral("contactUserIdInput"));
    userIdInput_->setPlaceholderText(QStringLiteral("输入联系人账号 ID"));
    userIdInput_->setClearButtonEnabled(true);

    panelLayout->addWidget(fieldLabel);
    panelLayout->addSpacing(10);
    panelLayout->addWidget(userIdInput_);
    panelLayout->addSpacing(24);

    auto* actionLayout = new QHBoxLayout();
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(10);
    actionLayout->addStretch(1);

    auto* cancelButton = new QPushButton(QStringLiteral("取消"), panel);
    cancelButton->setObjectName(QStringLiteral("addContactCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    confirmButton_ = new QPushButton(QStringLiteral("添加"), panel);
    confirmButton_->setObjectName(QStringLiteral("addContactConfirmButton"));
    confirmButton_->setCursor(Qt::PointingHandCursor);

    actionLayout->addWidget(cancelButton);
    actionLayout->addWidget(confirmButton_);
    panelLayout->addLayout(actionLayout);
    rootLayout->addWidget(panel);

    connect(userIdInput_, &QLineEdit::textChanged, this, [this] { updateConfirmButton(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] { reject(); });
    connect(confirmButton_, &QPushButton::clicked, this, [this] {
        if (!userId().isEmpty()) accept();
    });
}

void AddContactDialog::applyStyle() {
    setStyleSheet(QStringLiteral(R"(
        QDialog#addContactDialog {
            background: transparent;
        }
        #addContactPanel {
            background: #ffffff;
            border: 1px solid #d8e2f0;
            border-radius: 18px;
        }
        #addContactTitle {
            color: #0e1628;
            font-size: 24px;
            font-weight: 800;
            background: transparent;
        }
        #addContactSubtitle {
            color: #66758c;
            font-size: 13px;
            font-weight: 700;
            background: transparent;
        }
        #addContactFieldLabel {
            color: #172033;
            font-size: 15px;
            font-weight: 800;
            background: transparent;
        }
        #contactUserIdInput {
            min-height: 48px;
            border: 1px solid #d8e2f0;
            border-radius: 10px;
            background: #ffffff;
            color: #101828;
            padding: 0 14px;
            font-size: 16px;
        }
        #contactUserIdInput:focus {
            border-color: #42b8ff;
            background: #ffffff;
        }
        #addContactCancelButton,
        #addContactConfirmButton {
            min-width: 96px;
            min-height: 40px;
            border-radius: 10px;
            font-size: 14px;
            font-weight: 800;
        }
        #addContactCancelButton {
            color: #66758c;
            background: #f6f9fc;
            border: 1px solid #d8e2f0;
        }
        #addContactCancelButton:hover {
            background: #edf4fb;
        }
    )"));
}

void AddContactDialog::updateConfirmButton() {
    const bool canSubmit = !userId().isEmpty();
    confirmButton_->setEnabled(canSubmit);
    confirmButton_->setStyleSheet(canSubmit
                                      ? QStringLiteral("#addContactConfirmButton { background: #0f8dde; border: 0; color: #ffffff; } #addContactConfirmButton:hover { background: #087fc9; }")
                                      : QStringLiteral("#addContactConfirmButton { background: #acd2ea; border: 0; color: #e8f4fb; }"));
}
