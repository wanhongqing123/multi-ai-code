// 文件作用：设置页和 Agent 共用的本地密码表单；密码仅在控件和普通鉴权请求内使用。
// 不把密码写入工具参数、请求状态、诊断或本地设置；提交期间不能把关闭误报为取消。
#pragma once

#include <QDialog>
#include <QString>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;

class PasswordChangeDialog final : public QDialog {
    Q_OBJECT
public:
    using Completion = std::function<void(const QString& errorCode)>;
    using Submit = std::function<void(const QString& password, Completion)>;
    PasswordChangeDialog(const QString& account, Submit submit,
                         std::function<void()> shown, std::function<void()> canceled,
                         QWidget* parent = nullptr);
    void dismissForIdentityChange();
    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void submit();
    void setSubmitting(bool submitting);
    void finish(const QString& errorCode);
    void clearFields();
    Submit submit_;
    std::function<void()> shown_;
    std::function<void()> canceled_;
    QLineEdit* password_ = nullptr;
    QLineEdit* confirmation_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* cancel_ = nullptr;
    bool submitting_ = false;
    bool notifiedShown_ = false;
    bool finished_ = false;
};
