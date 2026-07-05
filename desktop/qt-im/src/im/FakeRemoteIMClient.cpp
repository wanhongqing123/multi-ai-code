#include "im/FakeRemoteIMClient.h"

FakeRemoteIMClient::FakeRemoteIMClient(QObject* parent) : RemoteIMClient(parent) {}

void FakeRemoteIMClient::connectToService(int, const QString& userId, const QString&, RemoteIMCompletion completion) {
    connectedUserId_ = userId.trimmed();
    complete(std::move(completion));
}

void FakeRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    connectedUserId_.clear();
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) {
    lastTextPeerId_ = peerId.trimmed();
    lastText_ = text.trimmed();
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) {
    lastImagePeerId_ = peerId.trimmed();
    lastImagePath_ = localPath.trimmed();
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    complete(std::move(completion));
}

QString FakeRemoteIMClient::connectedUserId() const { return connectedUserId_; }
QString FakeRemoteIMClient::lastTextPeerId() const { return lastTextPeerId_; }
QString FakeRemoteIMClient::lastText() const { return lastText_; }
QString FakeRemoteIMClient::lastImagePeerId() const { return lastImagePeerId_; }
QString FakeRemoteIMClient::lastImagePath() const { return lastImagePath_; }

void FakeRemoteIMClient::failNext(const QString& error) {
    nextError_ = error;
}

void FakeRemoteIMClient::emitIncomingText(const QString& fromUserId, const QString& text) {
    emit incomingText(fromUserId.trimmed(), text.trimmed());
}

void FakeRemoteIMClient::emitIncomingImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes) {
    emit incomingImage(fromUserId.trimmed(), localPath.trimmed(), width, height, sizeBytes);
}

void FakeRemoteIMClient::complete(RemoteIMCompletion completion) {
    if (!completion) return;
    if (nextError_.isEmpty()) {
        completion(true, QString());
        return;
    }
    const QString error = nextError_;
    nextError_.clear();
    completion(false, error);
}
