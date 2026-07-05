#pragma once

#include <QLibrary>

#include "im/TimSdkApi.h"

class DynamicTimSdkApi final : public TimSdkApi {
public:
    explicit DynamicTimSdkApi(QString libraryPath = libraryPathFromEnvironment());
    ~DynamicTimSdkApi() override;

    static QString libraryPathFromEnvironment();

    bool isReady() const override;
    QString diagnosticError() const override;

    int init(quint64 sdkAppId, const QString& jsonConfig) override;
    void uninit() override;
    int login(const QString& userId, const QString& userSig, TimSdkCompletion completion) override;
    int logout(TimSdkCompletion completion) override;
    int sendMessage(const QString& conversationId,
                    int conversationType,
                    const QString& jsonMessage,
                    TimSdkCompletion completion) override;
    int getConversationList(TimSdkCompletion completion) override;
    int getFriendList(TimSdkCompletion completion) override;
    int getMessageList(const QString& conversationId,
                       int conversationType,
                       const QString& jsonRequest,
                       TimSdkCompletion completion) override;
    void addReceiveMessageCallback(TimSdkReceiveMessagesCallback callback) override;
    void removeReceiveMessageCallback() override;

private:
    using NativeCompletion = void (*)(int code, const char* description, const char* jsonPayload, const void* userData);
    using NativeReceiveMessages = void (*)(const char* jsonMessages, const void* userData);
    using InitFn = int (*)(quint64 sdkAppId, const char* jsonConfig);
    using UninitFn = int (*)();
    using LoginFn = int (*)(const char* userId, const char* userSig, NativeCompletion completion, const void* userData);
    using LogoutFn = int (*)(NativeCompletion completion, const void* userData);
    using SendMessageFn = int (*)(const char* conversationId,
                                  int conversationType,
                                  const char* jsonMessage,
                                  char* messageIdBuffer,
                                  NativeCompletion completion,
                                  const void* userData);
    using GetConversationListFn = int (*)(NativeCompletion completion, const void* userData);
    using GetFriendListFn = int (*)(NativeCompletion completion, const void* userData);
    using GetMessageListFn = int (*)(const char* conversationId,
                                     int conversationType,
                                     const char* jsonRequest,
                                     NativeCompletion completion,
                                     const void* userData);
    using AddReceiveMessagesFn = void (*)(NativeReceiveMessages callback, const void* userData);
    using RemoveReceiveMessagesFn = void (*)(NativeReceiveMessages callback);

    template <typename T>
    T resolve(const char* symbol);

    static void completeOnce(int code, const char* description, const char* jsonPayload, const void* userData);
    static void receiveMessages(const char* jsonMessages, const void* userData);
    int completeIfImmediateFailure(int result, TimSdkCompletion completion);

    QLibrary library_;
    QString diagnosticError_;
    InitFn init_ = nullptr;
    UninitFn uninit_ = nullptr;
    LoginFn login_ = nullptr;
    LogoutFn logout_ = nullptr;
    SendMessageFn sendMessage_ = nullptr;
    GetConversationListFn getConversationList_ = nullptr;
    GetFriendListFn getFriendList_ = nullptr;
    GetMessageListFn getMessageList_ = nullptr;
    AddReceiveMessagesFn addReceiveMessages_ = nullptr;
    RemoveReceiveMessagesFn removeReceiveMessages_ = nullptr;
    TimSdkReceiveMessagesCallback receiveMessagesCallback_;
};
