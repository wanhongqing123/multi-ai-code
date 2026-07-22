#include "im/DynamicTimSdkApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>
#include <QtGlobal>
#include <utility>

namespace {

QString cleanEnv(const QString& name) {
    return QProcessEnvironment::systemEnvironment().value(name).trimmed();
}

QString firstExistingFile(const QStringList& paths) {
    for (const QString& path : paths) {
        if (QFileInfo::exists(path)) return path;
    }
    return {};
}

QStringList vendorRoots() {
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList roots = {
        QDir(appDir).filePath(QStringLiteral("vendor/tencent-im")),
        QDir(appDir).filePath(QStringLiteral("../vendor/tencent-im")),
        QDir(appDir).filePath(QStringLiteral("../../vendor/tencent-im")),
        QDir::current().filePath(QStringLiteral("MaiChat/desktop/vendor/tencent-im")),
        QDir::current().filePath(QStringLiteral("vendor/tencent-im"))
    };

    QDir dir(appDir);
    for (int i = 0; i < 8; ++i) {
        roots.append(dir.filePath(QStringLiteral("vendor/tencent-im")));
        roots.append(dir.filePath(QStringLiteral("MaiChat/desktop/vendor/tencent-im")));
        if (!dir.cdUp()) break;
    }

    roots.removeDuplicates();
    return roots;
}

QStringList defaultSdkLibraryCandidates() {
    QStringList candidates;
    for (const QString& root : vendorRoots()) {
#if defined(Q_OS_WIN)
#if QT_POINTER_SIZE == 8
        candidates.append(QDir(root).filePath(QStringLiteral("windows/shared_lib/Win64/ImSDK.dll")));
        candidates.append(QDir(root).filePath(QStringLiteral("windows/shared_lib/Win32/ImSDK.dll")));
#else
        candidates.append(QDir(root).filePath(QStringLiteral("windows/shared_lib/Win32/ImSDK.dll")));
        candidates.append(QDir(root).filePath(QStringLiteral("windows/shared_lib/Win64/ImSDK.dll")));
#endif
#elif defined(Q_OS_MACOS)
        candidates.append(QDir(root).filePath(QStringLiteral("macos/ImSDKForMac_Plus.framework/Versions/A/ImSDKForMac_Plus")));
        candidates.append(QDir(root).filePath(QStringLiteral("macos/ImSDKForMac_Plus.framework/ImSDKForMac_Plus")));
#endif
    }
    return candidates;
}

QString textFromC(const char* value) {
    return value ? QString::fromUtf8(value) : QString();
}

}  // namespace

DynamicTimSdkApi::DynamicTimSdkApi(QString libraryPath) : library_(std::move(libraryPath)) {
    if (library_.fileName().trimmed().isEmpty()) {
        diagnosticError_ = QStringLiteral("未配置桌面 IM SDK 动态库路径，请设置 MAICHAT_SDK_LIBRARY");
        return;
    }
    if (!library_.load()) {
        diagnosticError_ = library_.errorString();
        return;
    }

    init_ = resolve<InitFn>("TIMInit");
    uninit_ = resolve<UninitFn>("TIMUninit");
    login_ = resolve<LoginFn>("TIMLogin");
    logout_ = resolve<LogoutFn>("TIMLogout");
    sendMessage_ = resolve<SendMessageFn>("TIMMsgSendMessage");
    getConversationList_ = resolve<GetConversationListFn>("TIMConvGetConvList");
    getFriendList_ = resolve<GetFriendListFn>("TIMFriendshipGetFriendProfileList");
    deleteFriend_ = resolve<DeleteFriendFn>("TIMFriendshipDeleteFriend");
    deleteConversation_ = resolve<DeleteConversationFn>("TIMConvDelete");
    getMessageList_ = resolve<GetMessageListFn>("TIMMsgGetMsgList");
    addReceiveMessages_ = resolve<AddReceiveMessagesFn>("TIMAddRecvNewMsgCallback");
    removeReceiveMessages_ = resolve<RemoveReceiveMessagesFn>("TIMRemoveRecvNewMsgCallback");
}

DynamicTimSdkApi::~DynamicTimSdkApi() {
    removeReceiveMessageCallback();
}

QString DynamicTimSdkApi::libraryPathFromEnvironment() {
    const QString configuredPath = cleanEnv(QStringLiteral("MAICHAT_SDK_LIBRARY"));
    if (!configuredPath.isEmpty()) return configuredPath;
    return firstExistingFile(defaultSdkLibraryCandidates());
}

bool DynamicTimSdkApi::isReady() const {
    return init_ && uninit_ && login_ && logout_ && sendMessage_ && getConversationList_ && getFriendList_ &&
           deleteFriend_ && deleteConversation_ && getMessageList_ && addReceiveMessages_ && removeReceiveMessages_;
}

QString DynamicTimSdkApi::diagnosticError() const {
    if (!diagnosticError_.isEmpty()) return diagnosticError_;
    if (!isReady()) return QStringLiteral("桌面 IM SDK 动态库缺少必要的 C API 符号");
    return {};
}

int DynamicTimSdkApi::init(quint64 sdkAppId, const QString& jsonConfig) {
    if (!init_) return -1;
    return init_(sdkAppId, jsonConfig.toUtf8().constData());
}

void DynamicTimSdkApi::uninit() {
    if (uninit_) uninit_();
}

int DynamicTimSdkApi::login(const QString& userId, const QString& userSig, TimSdkCompletion completion) {
    if (!login_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = login_(userId.toUtf8().constData(), userSig.toUtf8().constData(), completeOnce, heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::logout(TimSdkCompletion completion) {
    if (!logout_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = logout_(completeOnce, heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::sendMessage(const QString& conversationId,
                                  int conversationType,
                                  const QString& jsonMessage,
                                  TimSdkCompletion completion) {
    if (!sendMessage_) return completeIfImmediateFailure(-1, std::move(completion));
    char messageIdBuffer[128] = {0};
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = sendMessage_(conversationId.toUtf8().constData(),
                                    conversationType,
                                    jsonMessage.toUtf8().constData(),
                                    messageIdBuffer,
                                    completeOnce,
                                    heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::getConversationList(TimSdkCompletion completion) {
    if (!getConversationList_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = getConversationList_(completeOnce, heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::getFriendList(TimSdkCompletion completion) {
    if (!getFriendList_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = getFriendList_(completeOnce, heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::deleteFriend(const QString& jsonRequest, TimSdkCompletion completion) {
    if (!deleteFriend_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = deleteFriend_(jsonRequest.toUtf8().constData(), completeOnce, heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::deleteConversation(const QString& conversationId,
                                         int conversationType,
                                         TimSdkCompletion completion) {
    if (!deleteConversation_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = deleteConversation_(conversationId.toUtf8().constData(),
                                           conversationType,
                                           completeOnce,
                                           heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

int DynamicTimSdkApi::getMessageList(const QString& conversationId,
                                     int conversationType,
                                     const QString& jsonRequest,
                                     TimSdkCompletion completion) {
    if (!getMessageList_) return completeIfImmediateFailure(-1, std::move(completion));
    auto* heapCompletion = new TimSdkCompletion(std::move(completion));
    const int result = getMessageList_(conversationId.toUtf8().constData(),
                                      conversationType,
                                      jsonRequest.toUtf8().constData(),
                                      completeOnce,
                                      heapCompletion);
    if (result != 0) {
        TimSdkCompletion failedCompletion = std::move(*heapCompletion);
        delete heapCompletion;
        return completeIfImmediateFailure(result, std::move(failedCompletion));
    }
    return result;
}

void DynamicTimSdkApi::addReceiveMessageCallback(TimSdkReceiveMessagesCallback callback) {
    receiveMessagesCallback_ = std::move(callback);
    if (addReceiveMessages_) addReceiveMessages_(receiveMessages, this);
}

void DynamicTimSdkApi::removeReceiveMessageCallback() {
    receiveMessagesCallback_ = nullptr;
    if (removeReceiveMessages_) removeReceiveMessages_(receiveMessages);
}

template <typename T>
T DynamicTimSdkApi::resolve(const char* symbol) {
    QFunctionPointer pointer = library_.resolve(symbol);
    if (!pointer && diagnosticError_.isEmpty()) {
        diagnosticError_ = QStringLiteral("桌面 IM SDK 动态库缺少符号：%1").arg(QString::fromUtf8(symbol));
    }
    return reinterpret_cast<T>(pointer);
}

void DynamicTimSdkApi::completeOnce(int code, const char* description, const char* jsonPayload, const void* userData) {
    auto* completion = reinterpret_cast<TimSdkCompletion*>(const_cast<void*>(userData));
    if (!completion) return;
    if (*completion) (*completion)(code, textFromC(description), textFromC(jsonPayload));
    delete completion;
}

void DynamicTimSdkApi::receiveMessages(const char* jsonMessages, const void* userData) {
    auto* api = reinterpret_cast<DynamicTimSdkApi*>(const_cast<void*>(userData));
    if (!api || !api->receiveMessagesCallback_) return;
    api->receiveMessagesCallback_(textFromC(jsonMessages));
}

int DynamicTimSdkApi::completeIfImmediateFailure(int result, TimSdkCompletion completion) {
    if (completion) completion(result, QStringLiteral("SDK 接口调用失败：%1").arg(result), QString());
    return result;
}
