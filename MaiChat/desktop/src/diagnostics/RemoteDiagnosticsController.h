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

// Owns one explicitly confirmed A -> B -> A -> C collection. The application
// send facade persists messages and binds destinations independently of the UI.
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
    // Injectable deadlines for deterministic tests; defaults match iOS.
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
