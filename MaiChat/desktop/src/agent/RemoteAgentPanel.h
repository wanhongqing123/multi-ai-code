// 文件作用：桌面端显示同账号其他设备的 Session；所有执行请求都回到所属设备。
#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>
#include <memory>

class RemoteAgentPanel final : public QWidget {
public:
    using Completion = std::function<void(const QString&)>;
    using CommandHandler = std::function<void(const QJsonObject&, const QJsonObject&, Completion)>;
    using HistoryHandler = std::function<void(const QJsonObject&, const QString&, int, Completion)>;

    explicit RemoteAgentPanel(QWidget* parent = nullptr);
    ~RemoteAgentPanel() override;

    void setCommandHandler(CommandHandler handler);
    void setHistoryHandler(HistoryHandler handler);
    void openSession(const QJsonObject& remote);
    void updateSession(const QJsonObject& remote);
    void receivePayload(const QJsonObject& remote, const QString& kind, const QJsonObject& payload);
    QJsonObject session() const;

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    void refreshHistory();
    void requestHistory(const QString& before, int limit);
    void sendOrStop();
    void sendAction(const QJsonObject& action, bool clearDraft);
    void removeOptimisticMessage(const QString& id);
    void renderMessages();
    void updateThinking();
    void renderRequests(const QJsonObject& payload);
    void updateControls();
    void showStatus(const QString& message, bool failure = false);
};
