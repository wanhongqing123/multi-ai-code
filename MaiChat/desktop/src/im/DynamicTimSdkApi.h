// 文件作用：桌面端IM 会话、消息与登录；DynamicTimSdkApi 负责腾讯 IM SDK 动态接口加载与调用，本文件给出其声明。
// 类型关系：DynamicTimSdkApi final : public TimSdkApi；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 DynamicTimSdkApi 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：libraryPathFromEnvironment、completeOnce、receiveMessages、selfInfoUpdated。
// 直接依赖：im/TimSdkApi.h；实现与这些接口协作。
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
    int getUserProfiles(const QString& request, TimSdkCompletion completion) override;
    void setSelfInfoUpdatedCallback(TimSdkReceiveMessagesCallback callback) override;
    int deleteFriend(const QString& jsonRequest, TimSdkCompletion completion) override;
    int deleteConversation(const QString& conversationId,
                           int conversationType,
                           TimSdkCompletion completion) override;
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
    using GetUserProfilesFn = int (*)(const char* request, NativeCompletion completion, const void* userData);
    using SetSelfInfoUpdatedFn = void (*)(NativeReceiveMessages callback, const void* userData);
    using DeleteFriendFn = int (*)(const char* jsonRequest, NativeCompletion completion, const void* userData);
    using DeleteConversationFn = int (*)(const char* conversationId,
                                         int conversationType,
                                         NativeCompletion completion,
                                         const void* userData);
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
    static void selfInfoUpdated(const char* profile, const void* userData);
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
    GetUserProfilesFn getUserProfiles_ = nullptr;
    SetSelfInfoUpdatedFn setSelfInfoUpdated_ = nullptr;
    DeleteFriendFn deleteFriend_ = nullptr;
    DeleteConversationFn deleteConversation_ = nullptr;
    GetMessageListFn getMessageList_ = nullptr;
    AddReceiveMessagesFn addReceiveMessages_ = nullptr;
    RemoveReceiveMessagesFn removeReceiveMessages_ = nullptr;
    TimSdkReceiveMessagesCallback receiveMessagesCallback_;
    TimSdkReceiveMessagesCallback selfInfoUpdatedCallback_;
};
