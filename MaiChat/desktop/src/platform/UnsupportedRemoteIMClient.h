// 文件作用：桌面端操作系统能力适配；UnsupportedRemoteIMClient 负责外部接口请求与结果转换，本文件给出其声明。
// 类型关系：UnsupportedRemoteIMClient : public RemoteIMClient；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 UnsupportedRemoteIMClient 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：im/RemoteIMClient.h；实现与这些接口协作。
#pragma once

#include "im/RemoteIMClient.h"

class UnsupportedRemoteIMClient : public RemoteIMClient {
public:
    explicit UnsupportedRemoteIMClient(QObject* parent = nullptr);

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig,
                          RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds,
                   RemoteIMCompletion completion) override;

protected:
    void fail(RemoteIMCompletion completion, const QString& action) const;
};
