// 文件作用：把本机 MaiAgent 会话映射为跨设备同步协议，并执行已鉴权的远端操作。
// 调用方必须先由账号服务验证发送方与本机属于同一个 MaiChat 账号。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class AgentController;

class DesktopSessionSyncHost final {
public:
    explicit DesktopSessionSyncHost(AgentController& controller);

    QJsonArray rootSessions() const;
    QJsonObject historyPage(const QString& sessionId, const QString& beforeId, int limit) const;
    QJsonObject applyCommand(const QString& sessionId, const QJsonObject& payload);

private:
    AgentController& controller_;
};
