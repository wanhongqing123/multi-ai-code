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
