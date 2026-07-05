#pragma once

#include <QString>
#include <functional>

using TimSdkCompletion = std::function<void(int code, const QString& description, const QString& jsonPayload)>;
using TimSdkReceiveMessagesCallback = std::function<void(const QString& jsonMessages)>;

class TimSdkApi {
public:
    virtual ~TimSdkApi() = default;

    virtual bool isReady() const { return true; }
    virtual QString diagnosticError() const { return {}; }

    virtual int init(quint64 sdkAppId, const QString& jsonConfig) = 0;
    virtual void uninit() = 0;
    virtual int login(const QString& userId, const QString& userSig, TimSdkCompletion completion) = 0;
    virtual int logout(TimSdkCompletion completion) = 0;
    virtual int sendMessage(const QString& conversationId,
                            int conversationType,
                            const QString& jsonMessage,
                            TimSdkCompletion completion) = 0;
    virtual int getConversationList(TimSdkCompletion completion) = 0;
    virtual int getFriendList(TimSdkCompletion completion) = 0;
    virtual int getMessageList(const QString& conversationId,
                               int conversationType,
                               const QString& jsonRequest,
                               TimSdkCompletion completion) = 0;
    virtual void addReceiveMessageCallback(TimSdkReceiveMessagesCallback callback) = 0;
    virtual void removeReceiveMessageCallback() = 0;
};
