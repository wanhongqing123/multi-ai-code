// 文件作用：原生安全输入密码并调用普通 setter；回调只接固定状态码，不回显密码。
#include "ui/PasswordChangeDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <utility>

#include "ui/UiZoom.h"

PasswordChangeDialog::PasswordChangeDialog(const QString& account, Submit submit,
    std::function<void()> shown, std::function<void()> canceled, QWidget* parent)
    : QDialog(parent), submit_(std::move(submit)), shown_(std::move(shown)),
      canceled_(std::move(canceled)) {
    setObjectName(QStringLiteral("passwordChangeDialog"));
    setWindowTitle(QStringLiteral("修改登录密码"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(UiZoom::s(430));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("passwordChangePanel"));
    root->addWidget(panel);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(28, 26, 28, 24);
    layout->setSpacing(UiZoom::s(12));
    auto* title = new QLabel(QStringLiteral("修改登录密码"), panel);
    title->setObjectName(QStringLiteral("passwordChangeTitle"));
    layout->addWidget(title);
    auto* description = new QLabel(QStringLiteral("账号：%1\n密码须为 15–128 个字符。修改后其他设备会退出登录，此电脑保持登录。")
                                       .arg(account), panel);
    description->setWordWrap(true);
    layout->addWidget(description);
    password_ = new QLineEdit(panel);
    confirmation_ = new QLineEdit(panel);
    password_->setObjectName(QStringLiteral("passwordChangeNew"));
    confirmation_->setObjectName(QStringLiteral("passwordChangeConfirm"));
    password_->setPlaceholderText(QStringLiteral("新密码"));
    confirmation_->setPlaceholderText(QStringLiteral("再次输入新密码"));
    for (QLineEdit* input : {password_, confirmation_}) {
        input->setEchoMode(QLineEdit::Password);
        input->setMaxLength(256); // 128 个补充平面码点可占 256 个 UTF-16 code unit。
        input->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
        input->setClearButtonEnabled(false);
        layout->addWidget(input);
    }
    error_ = new QLabel(panel);
    error_->setObjectName(QStringLiteral("passwordChangeError"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    cancel_ = new QPushButton(QStringLiteral("取消"), panel);
    save_ = new QPushButton(QStringLiteral("保存密码"), panel);
    cancel_->setObjectName(QStringLiteral("passwordChangeCancel"));
    save_->setObjectName(QStringLiteral("passwordChangeSave"));
    for (QPushButton* button : {cancel_, save_}) button->setMinimumSize(UiZoom::s(100), UiZoom::s(40));
    buttons->addWidget(cancel_);
    buttons->addWidget(save_);
    layout->addLayout(buttons);
    connect(cancel_, &QPushButton::clicked, this, &PasswordChangeDialog::reject);
    connect(save_, &QPushButton::clicked, this, &PasswordChangeDialog::submit);
    connect(confirmation_, &QLineEdit::returnPressed, this, &PasswordChangeDialog::submit);
    setStyleSheet(QStringLiteral(
        "#passwordChangePanel{background:#fff;border:1px solid #dbe5f1;border-radius:16px;}"
        "QLabel{color:#23324a;}#passwordChangeTitle{font-size:20px;font-weight:600;}"
        "QLineEdit{min-height:38px;padding:0 10px;border:1px solid #d5e0ee;border-radius:8px;background:#f8fbff;}"
        "QLineEdit:focus{border:1px solid #168ee0;}"
        "QPushButton{border:1px solid #d5e0ee;border-radius:8px;background:#f1f6fc;color:#23324a;}"
        "#passwordChangeSave{background:#168ee0;color:#fff;border:0;}"
        "QPushButton:disabled{color:#8b99aa;background:#e9eef5;}"
        "#passwordChangeError{color:#c43236;}"));
}

void PasswordChangeDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!notifiedShown_) {
        notifiedShown_ = true;
        if (shown_) shown_();
    }
    password_->setFocus();
}

void PasswordChangeDialog::clearFields() {
    password_->clear();
    confirmation_->clear();
}

void PasswordChangeDialog::setSubmitting(bool submitting) {
    submitting_ = submitting;
    password_->setEnabled(!submitting);
    confirmation_->setEnabled(!submitting);
    save_->setEnabled(!submitting);
    cancel_->setEnabled(!submitting);
    save_->setText(submitting ? QStringLiteral("正在保存…") : QStringLiteral("保存密码"));
}

void PasswordChangeDialog::submit() {
    if (submitting_ || finished_ || !submit_) return;
    QString password = password_->text();
    const int characters = password.toUcs4().size();
    if (characters < 15 || characters > 128 || password.toUtf8().size() > 512) {
        error_->setText(QStringLiteral("密码须为 15–128 个字符，且不超过 512 字节。"));
        error_->show();
        return;
    }
    if (password != confirmation_->text()) {
        error_->setText(QStringLiteral("两次输入的密码不一致。"));
        error_->show();
        return;
    }
    error_->hide();
    setSubmitting(true);
    clearFields();
    QPointer<PasswordChangeDialog> self(this);
    submit_(password, [self](const QString& code) { if (self) self->finish(code); });
    password.fill(QChar(0));
    password.clear();
}

void PasswordChangeDialog::finish(const QString& code) {
    setSubmitting(false);
    if (code.isEmpty()) {
        finished_ = true;
        accept();
        return;
    }
    QString message = QStringLiteral("密码修改失败，请稍后再试。密码未保留，请重新输入。");
    if (code == QStringLiteral("reauth_required"))
        message = QStringLiteral("请重新登录后，在 10 分钟内修改密码。");
    else if (code == QStringLiteral("identity_changed"))
        message = QStringLiteral("账号已变化，请关闭后从原账号重新打开。");
    else if (code == QStringLiteral("invalid_input"))
        message = QStringLiteral("密码格式不符合要求，请重新输入。");
    else if (code == QStringLiteral("network"))
        message = QStringLiteral("网络请求未确认成功，请稍后重试。");
    error_->setText(message);
    error_->show();
    if (code == QStringLiteral("identity_changed") || code == QStringLiteral("reauth_required"))
        save_->setEnabled(false);
}

void PasswordChangeDialog::reject() {
    if (submitting_) return;
    clearFields();
    if (!finished_ && canceled_) canceled_();
    finished_ = true;
    QDialog::reject();
}

void PasswordChangeDialog::dismissForIdentityChange() {
    finished_ = true;
    submitting_ = false;
    clearFields();
    QDialog::reject();
}
