// 文件作用：桌面端诊断信息采集与导出；RemoteDiagnosticsController 负责流程编排、异步回调和状态转换，本文件给出其声明。
// 类型关系：RemoteDiagnosticsController final : public QObject；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 RemoteDiagnosticsController 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：diagnostics/RemoteDiagnosticsEvidence.h；实现与这些接口协作。
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include "diagnostics/RemoteDiagnosticsEvidence.h"

class RemoteIMApplication;

// 管理一次经明确确认的 A → B → A → C 诊断收集。
// 应用发送门面独立于界面持久化消息并绑定收件目标。
class RemoteDiagnosticsController final : public QObject {
    Q_OBJECT
public:
    explicit RemoteDiagnosticsController(RemoteIMApplication* app, QObject* parent = nullptr);
    void start(const QString& peerId, const QString& recipientId);
    void cancel();
    bool isRunning() const { return running_; }
    bool isSending() const { return sending_; }
    QString status() const { return status_; }
    QString requestId() const { return requestId_; }
    // 截止时间可注入以便确定性测试；默认值与 iOS 保持一致。
    void setTimeouts(int responseMs, int sendMs);
signals:
    void changed();
private:
    bool contextValid() const;
    void poll();
    void finish(const QString& status);
    void sendReport(const QJsonObject& remote, const QString& missingReason);
    QJsonObject localEvidence() const;
    QPointer<RemoteIMApplication> app_;
    RemoteDiagnostics::AccountTag account_;
    QString peer_, recipient_, requestId_, status_;
    QString missingReason_;
    QJsonObject originalLocal_;
    QSet<QString> inspected_;
    QTimer timer_;
    QElapsedTimer elapsed_, sendingElapsed_;
    quint64 generation_ = 0;
    qint64 beganAt_ = 0;
    int responseMs_ = 60000, sendMs_ = 30000;
    bool running_ = false, sending_ = false, requestConfirmed_ = false;
};
