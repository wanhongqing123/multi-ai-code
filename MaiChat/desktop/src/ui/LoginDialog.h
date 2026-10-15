// 文件作用：桌面端共用界面；LoginDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：LoginDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 LoginDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
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
