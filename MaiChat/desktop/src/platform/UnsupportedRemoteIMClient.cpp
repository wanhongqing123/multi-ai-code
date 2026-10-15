// 文件作用：桌面端操作系统能力适配；UnsupportedRemoteIMClient 负责外部接口请求与结果转换，本文件给出其实现。
// 主要入口：connectToService、disconnectFromService、deleteContact、sendText。
// 实现边界：与 UnsupportedRemoteIMClient 的公开声明配套，调用方通过相应接口进入。
#include "platform/UnsupportedRemoteIMClient.h"

UnsupportedRemoteIMClient::UnsupportedRemoteIMClient(QObject* parent) : RemoteIMClient(parent) {}

void UnsupportedRemoteIMClient::connectToService(int, const QString&, const QString&, RemoteIMCompletion completion) {
    fail(std::move(completion), QStringLiteral("连接"));
}

void UnsupportedRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    if (completion) completion(true, QString());
}

void UnsupportedRemoteIMClient::deleteContact(const QString&, RemoteIMCompletion completion) {
    fail(std::move(completion), QStringLiteral("删除好友"));
}

void UnsupportedRemoteIMClient::sendText(const QString&, const QString&, RemoteIMSendCompletion completion) {
    fail([completion = std::move(completion)](bool ok, const QString& error) {
        if (completion) completion(ok, error, {});
    }, QStringLiteral("发送文本"));
}

void UnsupportedRemoteIMClient::sendImage(const QString&, const QString&, RemoteIMSendCompletion completion) {
    fail([completion = std::move(completion)](bool ok, const QString& error) {
        if (completion) completion(ok, error, {});
    }, QStringLiteral("发送图片"));
}

void UnsupportedRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    fail(std::move(completion), QStringLiteral("发送语音"));
}

void UnsupportedRemoteIMClient::fail(RemoteIMCompletion completion, const QString& action) const {
    if (completion) completion(false, QStringLiteral("%1需要接入当前平台的 IM SDK").arg(action));
}
