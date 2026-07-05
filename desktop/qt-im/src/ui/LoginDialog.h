#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>

class LoginDialog final : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(QWidget* parent = nullptr);

    QString userId() const;
    void setUserId(const QString& userId);

private:
    void buildUi();
    void applyStyle();
    void updateLoginButton();

    QLineEdit* userIdInput_ = nullptr;
    QPushButton* loginButton_ = nullptr;
};
