// 文件作用：本地密码表单的普通 open/status 入口，供 Settings 与 Agent 共用。
// 只保存 ID、原账号登录身份和状态；密码不进入此状态表，也不进入工具参数或结果。
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class QWidget;
class PasswordChangeDialog;

class DesktopPasswordChangeRequests final : public QObject {
public:
    using Completion = std::function<void(const QString& errorCode)>;
    using Submit = std::function<void(const QString& password, Completion)>;
    explicit DesktopPasswordChangeRequests(QWidget* window, QObject* parent = nullptr);
    void setHandlers(std::function<QString()> account, std::function<QString()> identity,
                     Submit submit);
    QJsonObject open();
    QJsonObject status(const QString& requestId);
    QJsonObject request(const QJsonObject& arguments);
    void cancelForAccountChange();

private:
    struct State {
        QString id;
        QString account;
        QString identity;
        QString status = QStringLiteral("opening");
        QString errorCode;
        bool opened = false;
    };
    bool canPresent() const;
    bool matchesIdentity(const State& state) const;
    QJsonObject result(const State& state) const;
    void showRequest(const QString& requestId);
    QPointer<QWidget> window_;
    QPointer<PasswordChangeDialog> dialog_;
    std::function<QString()> account_;
    std::function<QString()> identity_;
    Submit submit_;
    QHash<QString, State> requests_;
    QStringList order_;
    QString current_;
};
