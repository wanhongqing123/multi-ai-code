#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "im/TimSdkApi.h"
#include "im/TimSdkRemoteIMClient.h"

namespace {

class FakeTimSdkApi final : public TimSdkApi {
public:
    int init(quint64 sdkAppId, const QString& jsonConfig) override {
        initializedSdkAppId = sdkAppId;
        initConfig = jsonConfig;
        return initResult;
    }

    void uninit() override { uninitCalled = true; }

    int login(const QString& userId, const QString& userSig, TimSdkCompletion completion) override {
        loginUserId = userId;
        loginUserSig = userSig;
        if (completion) completion(loginCode, loginCode == 0 ? QString() : QStringLiteral("login failed"), QString());
        return loginResult;
    }

    int logout(TimSdkCompletion completion) override {
        logoutCalled = true;
        if (completion) completion(0, QString(), QString());
        return 0;
    }

    int sendMessage(const QString& conversationId,
                    int conversationType,
                    const QString& jsonMessage,
                    TimSdkCompletion completion) override {
        lastConversationId = conversationId;
        lastConversationType = conversationType;
        lastJsonMessage = jsonMessage;
        if (completion) completion(sendCode, sendCode == 0 ? QString() : QStringLiteral("send failed"), sendPayload);
        return sendResult;
    }

    int getConversationList(TimSdkCompletion completion) override {
        conversationListRequested = true;
        if (completion) completion(0, QString(), conversationListPayload);
        return 0;
    }

    int getFriendList(TimSdkCompletion completion) override {
        friendListRequested = true;
        if (completion) completion(0, QString(), friendListPayload);
        return 0;
    }

    int deleteFriend(const QString& jsonRequest, TimSdkCompletion completion) override {
        operations.append(QStringLiteral("deleteFriend"));
        deleteFriendRequest = jsonRequest;
        if (completion) completion(deleteFriendCode,
                                   deleteFriendCode == 0 ? QString() : QStringLiteral("delete friend failed"),
                                   QString());
        return deleteFriendCode;
    }

    int deleteConversation(const QString& conversationId,
                           int conversationType,
                           TimSdkCompletion completion) override {
        operations.append(QStringLiteral("deleteConversation"));
        deletedConversationId = conversationId;
        deletedConversationType = conversationType;
        if (completion) completion(deleteConversationCode,
                                   deleteConversationCode == 0 ? QString() : QStringLiteral("delete conversation failed"),
                                   QString());
        return deleteConversationCode;
    }

    int getMessageList(const QString& conversationId,
                       int conversationType,
                       const QString& jsonRequest,
                       TimSdkCompletion completion) override {
        historyConversationId = conversationId;
        historyConversationType = conversationType;
        historyRequest = jsonRequest;
        if (completion) completion(0, QString(), historyPayload);
        return 0;
    }

    void addReceiveMessageCallback(TimSdkReceiveMessagesCallback callback) override {
        receiveCallback = std::move(callback);
    }

    void removeReceiveMessageCallback() override { receiveCallback = nullptr; }

    void emitMessages(const QJsonArray& messages) {
        if (receiveCallback) {
            receiveCallback(QString::fromUtf8(QJsonDocument(messages).toJson(QJsonDocument::Compact)));
        }
    }

    int initResult = 0;
    int loginResult = 0;
    int loginCode = 0;
    int sendResult = 0;
    int sendCode = 0;
    quint64 initializedSdkAppId = 0;
    QString initConfig;
    QString loginUserId;
    QString loginUserSig;
    QString lastConversationId;
    int lastConversationType = 0;
    QString lastJsonMessage;
    QString sendPayload;
    bool conversationListRequested = false;
    bool friendListRequested = false;
    int deleteFriendCode = 0;
    int deleteConversationCode = 0;
    QString deleteFriendRequest;
    QString deletedConversationId;
    int deletedConversationType = 0;
    QStringList operations;
    QString conversationListPayload;
    QString friendListPayload;
    QString historyConversationId;
    int historyConversationType = 0;
    QString historyRequest;
    QString historyPayload;
    bool logoutCalled = false;
    bool uninitCalled = false;
    TimSdkReceiveMessagesCallback receiveCallback;
};

QJsonObject firstElement(const QString& jsonMessage) {
    const QJsonObject message = QJsonDocument::fromJson(jsonMessage.toUtf8()).object();
    return message.value(QStringLiteral("message_elem_array")).toArray().first().toObject();
}

QJsonObject messageMetadata(const QString& jsonMessage) {
    const QJsonObject message = QJsonDocument::fromJson(jsonMessage.toUtf8()).object();
    const QString metadataText =
        message.value(QStringLiteral("message_cloud_custom_str")).toString();
    return QJsonDocument::fromJson(metadataText.toUtf8()).object();
}

}  // namespace

class TimSdkRemoteIMClientTest : public QObject {
    Q_OBJECT

private slots:
    void connectsThroughSdkAndSendsTextAndImage();
    void sendsApprovalDecisionAsV2CloudInteraction();
    void sendsImageWithTextAsSingleMultiElemMessage();
    void sendsVideoWithEverySdkRequiredField();
    void sendsVideoWithTextAsSingleMultiElemMessage();
    void refusesVideoWithoutDurationOrCover();
    void deletesFriendAndConversationThroughSdk();
    void fetchesContactsConversationsAndHistoryAfterLogin();
    void emitsIncomingTextAndImageFromSdkMessages();
    void emitsIncomingApprovalRequestFromV2CloudMetadata();
    void emitsIncomingGenericFileWithRealMimeType();
    void mergesCaptionIntoGenericFileMessage();
    void loadsGenericFileFromHistory();
    void emitsIncomingVoiceWithLocalPath();
    void emitsIncomingVideoWithLocalPath();
    void mergesCaptionIntoIncomingVideoMessage();
    void rejectsMissingCredentials();
};

void TimSdkRemoteIMClientTest::connectsThroughSdkAndSendsTextAndImage() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));

    bool connected = false;
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), [&](bool ok, const QString&) {
        connected = ok;
    });

    QVERIFY(connected);
    QCOMPARE(fake->initializedSdkAppId, 123456ULL);
    QCOMPARE(fake->loginUserId, QStringLiteral("desktop-user"));
    QCOMPARE(fake->loginUserSig, QStringLiteral("sig-value"));
    QVERIFY(fake->receiveCallback != nullptr);

    bool textSent = false;
    RemoteIMSendReceipt sentReceipt;
    fake->sendPayload = QStringLiteral(
        "{\"message_msg_id\":\"sdk-sent-1\",\"message_server_time\":1700000001}");
    client.sendText(QStringLiteral("phone-user"), QStringLiteral("hello\nworld"), [&](bool ok, const QString&, const RemoteIMSendReceipt& receipt) {
        textSent = ok;
        sentReceipt = receipt;
    });
    QVERIFY(textSent);
    // 发送回执带 SDK 稳定 id 与服务端顺序（和漫游/实时投递使用同一排序键）。
    QCOMPARE(sentReceipt.remoteMessageId, QStringLiteral("sdk-sent-1#0"));
    QCOMPARE(sentReceipt.createdAtMillis, Q_INT64_C(1700000001) * 1000);
    QCOMPARE(fake->lastConversationId, QStringLiteral("phone-user"));
    QCOMPARE(fake->lastConversationType, 1);
    QJsonObject elem = firstElement(fake->lastJsonMessage);
    QCOMPARE(elem.value(QStringLiteral("elem_type")).toInt(), 0);
    QCOMPARE(elem.value(QStringLiteral("text_elem_content")).toString(), QStringLiteral("hello\nworld"));
    QJsonObject metadata = messageMetadata(fake->lastJsonMessage);
    QCOMPARE(metadata.value(QStringLiteral("namespace")).toString(), QStringLiteral("multi-ai-code"));
    QCOMPARE(metadata.value(QStringLiteral("version")).toInt(), 2);
    QCOMPARE(metadata.value(QStringLiteral("origin")).toString(), QStringLiteral("human"));

    // Remote-desktop/control frames use the same text transport but must not
    // be mistaken for human chat input by an AICLI receiver.
    client.sendMachineText(QStringLiteral("phone-user"), QStringLiteral("protocol-frame"), {});
    metadata = messageMetadata(fake->lastJsonMessage);
    QCOMPARE(metadata.value(QStringLiteral("origin")).toString(), QStringLiteral("machine"));

    bool imageSent = false;
    client.sendImage(QStringLiteral("phone-user"), QStringLiteral("/tmp/outgoing.png"), [&](bool ok, const QString&, const RemoteIMSendReceipt&) {
        imageSent = ok;
    });
    QVERIFY(imageSent);
    QCOMPARE(fake->lastConversationId, QStringLiteral("phone-user"));
    elem = firstElement(fake->lastJsonMessage);
    QCOMPARE(elem.value(QStringLiteral("elem_type")).toInt(), 1);
    QCOMPARE(elem.value(QStringLiteral("image_elem_orig_path")).toString(), QStringLiteral("/tmp/outgoing.png"));
    QCOMPARE(elem.value(QStringLiteral("image_elem_level")).toInt(), 0);
    metadata = messageMetadata(fake->lastJsonMessage);
    QCOMPARE(metadata.value(QStringLiteral("origin")).toString(), QStringLiteral("human"));
}

void TimSdkRemoteIMClientTest::sendsApprovalDecisionAsV2CloudInteraction() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));

    bool sent = false;
    client.sendApprovalDecision(
        QStringLiteral("phone-user"),
        QStringLiteral("approval-desktop-1"),
        RemoteIMApprovalAction::ApproveOnce,
        [&](bool ok, const QString&, const RemoteIMSendReceipt&) { sent = ok; });

    QVERIFY(sent);
    QCOMPARE(
        firstElement(fake->lastJsonMessage).value(QStringLiteral("text_elem_content")).toString(),
        QStringLiteral("审批操作：同意本次"));
    const QJsonObject metadata = messageMetadata(fake->lastJsonMessage);
    QCOMPARE(metadata.value(QStringLiteral("version")).toInt(), 2);
    QCOMPARE(metadata.value(QStringLiteral("origin")).toString(), QStringLiteral("human"));
    const QJsonObject interaction = metadata.value(QStringLiteral("interaction")).toObject();
    QCOMPARE(interaction.value(QStringLiteral("kind")).toString(),
             QStringLiteral("approval-decision"));
    QCOMPARE(interaction.value(QStringLiteral("token")).toString(),
             QStringLiteral("approval-desktop-1"));
    QCOMPARE(interaction.value(QStringLiteral("action")).toString(),
             QStringLiteral("approve-once"));
}

void TimSdkRemoteIMClientTest::sendsImageWithTextAsSingleMultiElemMessage() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);

    bool sent = false;
    client.sendImageWithText(QStringLiteral("phone-user"), QStringLiteral("/tmp/outgoing.png"), QStringLiteral("看这张图"),
                             [&](bool ok, const QString&, const RemoteIMSendReceipt&) { sent = ok; });
    QVERIFY(sent);
    QCOMPARE(fake->lastConversationId, QStringLiteral("phone-user"));
    QCOMPARE(fake->lastConversationType, 1);

    // 图片 + 配文合并成一条多元素消息：message_elem_array = [图片元素, 文本元素]。
    const QJsonObject message = QJsonDocument::fromJson(fake->lastJsonMessage.toUtf8()).object();
    const QJsonArray elems = message.value(QStringLiteral("message_elem_array")).toArray();
    QCOMPARE(elems.size(), 2);
    const QJsonObject imageElem = elems.at(0).toObject();
    QCOMPARE(imageElem.value(QStringLiteral("elem_type")).toInt(), 1);
    QCOMPARE(imageElem.value(QStringLiteral("image_elem_orig_path")).toString(), QStringLiteral("/tmp/outgoing.png"));
    QCOMPARE(imageElem.value(QStringLiteral("image_elem_level")).toInt(), 0);
    const QJsonObject textElem = elems.at(1).toObject();
    QCOMPARE(textElem.value(QStringLiteral("elem_type")).toInt(), 0);
    QCOMPARE(textElem.value(QStringLiteral("text_elem_content")).toString(), QStringLiteral("看这张图"));
}

namespace {

RemoteIMVideoPayload samplePayload() {
    RemoteIMVideoPayload video;
    video.videoPath = QStringLiteral("/tmp/screen-record.mp4");
    video.videoType = QStringLiteral("mp4");
    video.videoSizeBytes = 8 * 1024 * 1024;
    video.durationSeconds = 42;
    video.coverPath = QStringLiteral("/tmp/cover-1.jpg");
    video.coverType = QStringLiteral("jpg");
    video.coverSizeBytes = 51200;
    video.coverWidth = 1920;
    video.coverHeight = 1080;
    return video;
}

}  // namespace

void TimSdkRemoteIMClientTest::sendsVideoWithEverySdkRequiredField() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);

    bool sent = false;
    client.sendVideo(QStringLiteral("phone-user"), samplePayload(),
                     [&](bool ok, const QString&, const RemoteIMSendReceipt&) { sent = ok; });

    QVERIFY(sent);
    QCOMPARE(fake->lastConversationId, QStringLiteral("phone-user"));
    QCOMPARE(fake->lastConversationType, 1);
    const QJsonObject elem = firstElement(fake->lastJsonMessage);
    // TIMElemType::kTIMElem_Video == 9。发错枚举 SDK 不会报错，只会发出一条对端解不出的消息。
    QCOMPARE(elem.value(QStringLiteral("elem_type")).toInt(), 9);
    QCOMPARE(elem.value(QStringLiteral("video_elem_video_path")).toString(),
             QStringLiteral("/tmp/screen-record.mp4"));
    QCOMPARE(elem.value(QStringLiteral("video_elem_video_type")).toString(), QStringLiteral("mp4"));
    QCOMPARE(elem.value(QStringLiteral("video_elem_video_size")).toInt(), 8 * 1024 * 1024);
    QCOMPARE(elem.value(QStringLiteral("video_elem_video_duration")).toInt(), 42);
    QCOMPARE(elem.value(QStringLiteral("video_elem_image_path")).toString(),
             QStringLiteral("/tmp/cover-1.jpg"));
    QCOMPARE(elem.value(QStringLiteral("video_elem_image_type")).toString(), QStringLiteral("jpg"));
    QCOMPARE(elem.value(QStringLiteral("video_elem_image_size")).toInt(), 51200);
    QCOMPARE(elem.value(QStringLiteral("video_elem_image_width")).toInt(), 1920);
    QCOMPARE(elem.value(QStringLiteral("video_elem_image_height")).toInt(), 1080);
    // 尺寸字段必须是整数字面量：SDK 那边用 jsoncpp 的 asUInt() 读，
    // 序列化成 8388608.0 这种带小数点的形式会读废。
    QVERIFY(fake->lastJsonMessage.contains(QStringLiteral("\"video_elem_video_size\":8388608")));
    const QJsonObject metadata = messageMetadata(fake->lastJsonMessage);
    QCOMPARE(metadata.value(QStringLiteral("origin")).toString(), QStringLiteral("human"));
}

void TimSdkRemoteIMClientTest::sendsVideoWithTextAsSingleMultiElemMessage() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);

    bool sent = false;
    client.sendVideoWithText(QStringLiteral("phone-user"), samplePayload(), QStringLiteral("录屏在这"),
                             [&](bool ok, const QString&, const RemoteIMSendReceipt&) { sent = ok; });

    QVERIFY(sent);
    const QJsonObject message = QJsonDocument::fromJson(fake->lastJsonMessage.toUtf8()).object();
    const QJsonArray elems = message.value(QStringLiteral("message_elem_array")).toArray();
    QCOMPARE(elems.size(), 2);
    QCOMPARE(elems.at(0).toObject().value(QStringLiteral("elem_type")).toInt(), 9);
    QCOMPARE(elems.at(1).toObject().value(QStringLiteral("elem_type")).toInt(), 0);
    QCOMPARE(elems.at(1).toObject().value(QStringLiteral("text_elem_content")).toString(),
             QStringLiteral("录屏在这"));
}

void TimSdkRemoteIMClientTest::refusesVideoWithoutDurationOrCover() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->lastJsonMessage.clear();

    // 封面字段缺失时必须当场失败并说明原因。放行的话 SDK 照发不误，
    // 对端收到的是一条打不开的空视频——这类静默失败最难查。
    RemoteIMVideoPayload broken = samplePayload();
    broken.coverPath.clear();
    bool ok = true;
    QString error;
    client.sendVideo(QStringLiteral("phone-user"), broken,
                     [&](bool sent, const QString& message, const RemoteIMSendReceipt&) {
                         ok = sent;
                         error = message;
                     });

    QVERIFY(!ok);
    QVERIFY(!error.isEmpty());
    QVERIFY(fake->lastJsonMessage.isEmpty());
}

void TimSdkRemoteIMClientTest::deletesFriendAndConversationThroughSdk() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);

    bool deleted = false;
    client.deleteContact(QStringLiteral(" phone-user "), [&](bool ok, const QString&) {
        deleted = ok;
    });

    QVERIFY(deleted);
    QCOMPARE(fake->operations, QStringList({QStringLiteral("deleteFriend"), QStringLiteral("deleteConversation")}));
    const QJsonObject request = QJsonDocument::fromJson(fake->deleteFriendRequest.toUtf8()).object();
    QCOMPARE(request.value(QStringLiteral("friendship_delete_friend_param_friend_type")).toInt(), 1);
    QCOMPARE(request.value(QStringLiteral("friendship_delete_friend_param_identifier_array")).toArray(),
             QJsonArray({QStringLiteral("phone-user")}));
    QCOMPARE(fake->deletedConversationId, QStringLiteral("phone-user"));
    QCOMPARE(fake->deletedConversationType, 1);
}

void TimSdkRemoteIMClientTest::fetchesContactsConversationsAndHistoryAfterLogin() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    fake->friendListPayload = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{
            {QStringLiteral("friend_profile_identifier"), QStringLiteral("phone-user")},
            {QStringLiteral("friend_profile_remark"), QStringLiteral("手机")},
            {QStringLiteral("friend_profile_user_profile"), QJsonObject{
                {QStringLiteral("user_profile_nick_name"), QStringLiteral("iPhone")},
                {QStringLiteral("user_profile_face_url"), QStringLiteral("https://example.com/iphone.png")}
            }}
        }
    }).toJson(QJsonDocument::Compact));
    fake->conversationListPayload = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{
            {QStringLiteral("conv_id"), QStringLiteral("phone-user")},
            {QStringLiteral("conv_type"), 1},
            {QStringLiteral("conv_face_url"), QStringLiteral("https://example.com/conversation.png")}
        }
    }).toJson(QJsonDocument::Compact));
    fake->historyPayload = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{
            {QStringLiteral("message_is_from_self"), true},
            {QStringLiteral("message_sender"), QStringLiteral("desktop-user")},
            {QStringLiteral("message_conv_id"), QStringLiteral("phone-user")},
            {QStringLiteral("message_server_time"), 1700000000},
            {QStringLiteral("message_elem_array"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("elem_type"), 0},
                    {QStringLiteral("text_elem_content"), QStringLiteral("我发过的历史")}
                }
            }}
        },
        QJsonObject{
            {QStringLiteral("message_is_from_self"), false},
            {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
            {QStringLiteral("message_conv_id"), QStringLiteral("phone-user")},
            {QStringLiteral("message_server_time"), 1700000000},
            {QStringLiteral("message_elem_array"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("elem_type"), 0},
                    {QStringLiteral("text_elem_content"), QStringLiteral("历史消息")}
                }
            }}
        }
    }).toJson(QJsonDocument::Compact));
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy contactsSpy(&client, &RemoteIMClient::contactsReceived);
    QSignalSpy messagesSpy(&client, &RemoteIMClient::messagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);

    QVERIFY(fake->friendListRequested);
    QVERIFY(fake->conversationListRequested);
    QCOMPARE(fake->historyConversationId, QStringLiteral("phone-user"));
    QCOMPARE(fake->historyConversationType, 1);
    const QJsonObject historyRequest = QJsonDocument::fromJson(fake->historyRequest.toUtf8()).object();
    QCOMPARE(historyRequest.value(QStringLiteral("msg_getmsglist_param_count")).toInt(), 20);
    QCOMPARE(historyRequest.value(QStringLiteral("msg_getmsglist_param_is_ramble")).toBool(), true);
    QCOMPARE(historyRequest.value(QStringLiteral("msg_getmsglist_param_is_forward")).toBool(), false);

    QCOMPARE(contactsSpy.count(), 2);
    const QList<RemoteIMContact> friendContacts = qvariant_cast<QList<RemoteIMContact>>(contactsSpy.takeFirst().at(0));
    QCOMPARE(friendContacts.size(), 1);
    QCOMPARE(friendContacts.first().userId, QStringLiteral("phone-user"));
    QCOMPARE(friendContacts.first().displayName, QStringLiteral("手机"));
    QCOMPARE(friendContacts.first().avatarUrl, QStringLiteral("https://example.com/iphone.png"));
    const QList<RemoteIMContact> conversationContacts = qvariant_cast<QList<RemoteIMContact>>(contactsSpy.takeFirst().at(0));
    QCOMPARE(conversationContacts.size(), 1);
    QCOMPARE(conversationContacts.first().userId, QStringLiteral("phone-user"));
    QCOMPARE(conversationContacts.first().avatarUrl,
             QStringLiteral("https://example.com/conversation.png"));
    QCOMPARE(messagesSpy.count(), 1);
    const QList<RemoteIMMessage> messages = qvariant_cast<QList<RemoteIMMessage>>(messagesSpy.takeFirst().at(0));
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).direction, RemoteIMMessageDirection::Incoming);
    QCOMPARE(messages.at(0).fromUserId, QStringLiteral("phone-user"));
    QCOMPARE(messages.at(0).text, QStringLiteral("历史消息"));
    QCOMPARE(messages.at(1).direction, RemoteIMMessageDirection::Outgoing);
    QCOMPARE(messages.at(1).fromUserId, QStringLiteral("desktop-user"));
    QCOMPARE(messages.at(1).toUserId, QStringLiteral("phone-user"));
    QCOMPARE(messages.at(1).text, QStringLiteral("我发过的历史"));
    QVERIFY(messages.at(0).createdAtMillis < messages.at(1).createdAtMillis);
}

void TimSdkRemoteIMClientTest::emitsIncomingTextAndImageFromSdkMessages() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    // 实时消息经 liveMessagesReceived 通道送出：与漫游（messagesReceived）同构、
    // 携带稳定 SDK 消息 id（<msg_id>#<elem下标>）供落库去重，但会累计未读红点。
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);
    QSignalSpy roamingSpy(&client, &RemoteIMClient::messagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-msg-1")},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_cloud_custom_str"),
         QStringLiteral("{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"human\"}")},
        {QStringLiteral("message_elem_array"), QJsonArray{
            QJsonObject{
                {QStringLiteral("elem_type"), 0},
                {QStringLiteral("text_elem_content"), QStringLiteral("hi")}
            },
            QJsonObject{
                {QStringLiteral("elem_type"), 1},
                {QStringLiteral("image_elem_orig_path"), QStringLiteral("/tmp/incoming.png")},
                {QStringLiteral("image_elem_orig_pic_width"), 640},
                {QStringLiteral("image_elem_orig_pic_height"), 480},
                {QStringLiteral("image_elem_orig_pic_size"), 128}
            }
        }}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    // 图片 + 配文属于同一条 SDK 消息，合并成「一条」RemoteIMMessage（图片承载配文），
    // 稳定 id 锚定在图片元素上（#1），配文文本不再单独成条。
    QCOMPARE(messages.size(), 1);

    const RemoteIMMessage& image = messages.at(0);
    QCOMPARE(image.id, QStringLiteral("sdk-msg-1#1"));
    QCOMPARE(image.fromUserId, QStringLiteral("phone-user"));
    QCOMPARE(image.toUserId, QStringLiteral("desktop-user"));
    QCOMPARE(image.direction, RemoteIMMessageDirection::Incoming);
    QCOMPARE(image.origin, RemoteIMMessageOrigin::Human);
    QCOMPARE(image.text, QStringLiteral("hi"));
    QVERIFY(image.hasImage);
    QCOMPARE(image.image.localPath, QStringLiteral("/tmp/incoming.png"));
    QCOMPARE(image.image.width, 640);
    QCOMPARE(image.image.height, 480);
    QCOMPARE(image.image.sizeBytes, static_cast<qint64>(128));
    QCOMPARE(image.createdAtMillis, Q_INT64_C(1700000000) * 1000);
    // 实时推送不得串入漫游通道（否则未读红点永远不累计）。
    QCOMPARE(roamingSpy.count(), 0);
}

void TimSdkRemoteIMClientTest::emitsIncomingApprovalRequestFromV2CloudMetadata() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("multi-ai-code")},
        {QStringLiteral("message_msg_id"), QStringLiteral("approval-request-1")},
        {QStringLiteral("message_cloud_custom_str"), QStringLiteral(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"machine\","
            "\"interaction\":{\"kind\":\"approval-request\",\"token\":\"approval-desktop-2\","
            "\"actions\":[\"approve-once\",\"approve-prefix\",\"reject\"]}}")},
        {QStringLiteral("message_elem_array"), QJsonArray{QJsonObject{
            {QStringLiteral("elem_type"), 0},
            {QStringLiteral("text_elem_content"), QStringLiteral("Codex 请求执行一条高风险命令")}
        }}}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    QCOMPARE(messages.size(), 1);
    QVERIFY(messages.first().hasApprovalRequest);
    QCOMPARE(messages.first().approvalRequest.token, QStringLiteral("approval-desktop-2"));
    QVERIFY(messages.first().approvalRequest.actions
            == QList<RemoteIMApprovalAction>({RemoteIMApprovalAction::ApproveOnce,
                                              RemoteIMApprovalAction::ApprovePrefix,
                                              RemoteIMApprovalAction::Reject}));

    // 用户明确不兼容旧协议：v1 即使伪装成相同正文，也只能作为普通未知来源文本。
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("multi-ai-code")},
        {QStringLiteral("message_msg_id"), QStringLiteral("approval-request-v1")},
        {QStringLiteral("message_cloud_custom_str"), QStringLiteral(
            "{\"namespace\":\"multi-ai-code\",\"version\":1,\"origin\":\"machine\","
            "\"interaction\":{\"kind\":\"approval-request\",\"token\":\"approval-old\","
            "\"actions\":[\"approve-once\",\"reject\"]}}")},
        {QStringLiteral("message_elem_array"), QJsonArray{QJsonObject{
            {QStringLiteral("elem_type"), 0},
            {QStringLiteral("text_elem_content"), QStringLiteral("旧审批协议")}
        }}}
    }});
    QCOMPARE(messagesSpy.count(), 1);
    const auto oldMessages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    QCOMPARE(oldMessages.size(), 1);
    QVERIFY(!oldMessages.first().hasApprovalRequest);
    QCOMPARE(oldMessages.first().origin, RemoteIMMessageOrigin::Unknown);
}

// 普通文件（非 md/html）曾被接收解析层的白名单直接丢弃：消息根本不会生成，
// 表现为发送端一切正常、接收端毫无反应。这里钉住「任意扩展名都能收下」。
void TimSdkRemoteIMClientTest::emitsIncomingGenericFileWithRealMimeType() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-file-1")},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"), QJsonArray{
            QJsonObject{
                {QStringLiteral("elem_type"), 4},
                {QStringLiteral("file_elem_file_path"), QStringLiteral("/tmp/maichat-20260811.log")},
                {QStringLiteral("file_elem_file_name"), QStringLiteral("maichat-20260811.log")},
                {QStringLiteral("file_elem_file_size"), 88966}
            }
        }}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& file = messages.at(0);
    QVERIFY(file.hasFile);
    QCOMPARE(file.id, QStringLiteral("sdk-file-1#0"));
    QCOMPARE(file.file.localPath, QStringLiteral("/tmp/maichat-20260811.log"));
    QCOMPARE(file.file.fileName, QStringLiteral("maichat-20260811.log"));
    QCOMPARE(file.file.sizeBytes, static_cast<qint64>(88966));
    QCOMPARE(file.text, QStringLiteral("[文件消息] maichat-20260811.log"));
    // MIME 必须如实：谎报 text/markdown 会让 MainWindow 把它当文档去渲染，而不是走「另存为」。
    QCOMPARE(file.file.mimeType, QStringLiteral("text/plain"));
}

// 配文合并此前也挂在同一道白名单上：zip + 配文会退化成「只剩配文的纯文本消息」，附件整条消失。
void TimSdkRemoteIMClientTest::mergesCaptionIntoGenericFileMessage() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-file-2")},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"), QJsonArray{
            QJsonObject{
                {QStringLiteral("elem_type"), 4},
                {QStringLiteral("file_elem_file_path"), QStringLiteral("/tmp/logs.zip")},
                {QStringLiteral("file_elem_file_name"), QStringLiteral("logs.zip")},
                {QStringLiteral("file_elem_file_size"), 2048}
            },
            QJsonObject{
                {QStringLiteral("elem_type"), 0},
                {QStringLiteral("text_elem_content"), QStringLiteral("看下这个包")}
            }
        }}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    // 附件 + 配文合并成一条，配文不再单独成条。
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& file = messages.at(0);
    QVERIFY(file.hasFile);
    QCOMPARE(file.text, QStringLiteral("看下这个包"));
    QCOMPARE(file.file.fileName, QStringLiteral("logs.zip"));
    QCOMPARE(file.file.mimeType, QStringLiteral("application/zip"));
}

// 漫游/历史是与实时并列的第二条接收路径，此前有一模一样的白名单，必须一起放开。
void TimSdkRemoteIMClientTest::loadsGenericFileFromHistory() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    fake->conversationListPayload = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{
            {QStringLiteral("conv_id"), QStringLiteral("phone-user")},
            {QStringLiteral("conv_type"), 1}
        }
    }).toJson(QJsonDocument::Compact));
    fake->historyPayload = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{
            {QStringLiteral("message_is_from_self"), false},
            {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
            {QStringLiteral("message_conv_id"), QStringLiteral("phone-user")},
            {QStringLiteral("message_msg_id"), QStringLiteral("sdk-history-file")},
            {QStringLiteral("message_server_time"), 1700000000},
            {QStringLiteral("message_elem_array"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("elem_type"), 4},
                    {QStringLiteral("file_elem_file_path"), QStringLiteral("/tmp/report.pdf")},
                    {QStringLiteral("file_elem_file_name"), QStringLiteral("report.pdf")},
                    {QStringLiteral("file_elem_file_size"), 4096}
                }
            }}
        }
    }).toJson(QJsonDocument::Compact));
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::messagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& file = messages.at(0);
    QVERIFY(file.hasFile);
    QCOMPARE(file.id, QStringLiteral("sdk-history-file#0"));
    QCOMPARE(file.file.localPath, QStringLiteral("/tmp/report.pdf"));
    QCOMPARE(file.file.fileName, QStringLiteral("report.pdf"));
    QCOMPARE(file.file.mimeType, QStringLiteral("application/pdf"));
    QCOMPARE(file.text, QStringLiteral("[文件消息] report.pdf"));
}

// 语音入站此前完全没有分支：elem_type 2 一个 if 都匹配不上，整条消息静默消失，
// 界面上什么都不出现——而 incomingVoice 信号、ChatState::receiveVoice、本地库的
// voice_* 列全都早就备好了，只差这一环。
void TimSdkRemoteIMClientTest::emitsIncomingVoiceWithLocalPath() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-voice-1")},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"), QJsonArray{
            QJsonObject{
                {QStringLiteral("elem_type"), 2},
                {QStringLiteral("sound_elem_file_path"), QStringLiteral("/tmp/voice-1.amr")},
                {QStringLiteral("sound_elem_file_time"), 7}
            }
        }}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& voice = messages.at(0);
    QVERIFY(voice.hasVoice);
    QVERIFY(!voice.hasFile);
    QCOMPARE(voice.id, QStringLiteral("sdk-voice-1#0"));
    QCOMPARE(voice.voice.localPath, QStringLiteral("/tmp/voice-1.amr"));
    QCOMPARE(voice.voice.durationSeconds, 7);
    QCOMPARE(voice.text, QStringLiteral("[语音消息]"));
}

// 视频入站此前完全没有分支：elem_type 9 一个 if 都匹配不上，整条消息静默消失，
// 界面上什么都不出现。这条守住「收得到」。
void TimSdkRemoteIMClientTest::emitsIncomingVideoWithLocalPath() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-video-1")},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"), QJsonArray{
            QJsonObject{
                {QStringLiteral("elem_type"), 9},
                {QStringLiteral("video_elem_video_path"), QStringLiteral("/tmp/screen-record.mp4")},
                {QStringLiteral("video_elem_video_duration"), 42},
                {QStringLiteral("video_elem_video_size"), 8388608}
            }
        }}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& video = messages.at(0);
    QVERIFY(video.hasVideo);
    QVERIFY(!video.hasFile);
    QCOMPARE(video.id, QStringLiteral("sdk-video-1#0"));
    QCOMPARE(video.video.localPath, QStringLiteral("/tmp/screen-record.mp4"));
    QCOMPARE(video.video.durationSeconds, 42);
    QCOMPARE(video.video.sizeBytes, static_cast<qint64>(8388608));
    // 视频元素不带文件名，得从路径抠出来，否则气泡上没有标题。
    QCOMPARE(video.video.fileName, QStringLiteral("screen-record.mp4"));
    QCOMPARE(video.text, QStringLiteral("[视频消息] screen-record.mp4"));
}

// 与图片/文件同样的坑：视频 + 配文若不合并，会退化成「只剩配文的纯文本消息」，
// 视频整条消失，而用户以为发到了。
void TimSdkRemoteIMClientTest::mergesCaptionIntoIncomingVideoMessage() {
    auto api = std::make_unique<FakeTimSdkApi>();
    auto* fake = api.get();
    TimSdkRemoteIMClient client(std::move(api));
    QSignalSpy messagesSpy(&client, &RemoteIMClient::liveMessagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-video-2")},
        {QStringLiteral("message_server_time"), 1700000000},
        {QStringLiteral("message_elem_array"), QJsonArray{
            QJsonObject{
                {QStringLiteral("elem_type"), 9},
                {QStringLiteral("video_elem_video_path"), QStringLiteral("/tmp/clip.mp4")},
                {QStringLiteral("video_elem_video_duration"), 7},
                {QStringLiteral("video_elem_video_size"), 1024}
            },
            QJsonObject{
                {QStringLiteral("elem_type"), 0},
                {QStringLiteral("text_elem_content"), QStringLiteral("看下这段录屏")}
            }
        }}
    }});

    QCOMPARE(messagesSpy.count(), 1);
    const auto messages = messagesSpy.takeFirst().at(0).value<QList<RemoteIMMessage>>();
    // 只应产生一条：视频带配文，而不是拆成「视频」+「文本」两条。
    QCOMPARE(messages.size(), 1);
    QVERIFY(messages.at(0).hasVideo);
    QCOMPARE(messages.at(0).text, QStringLiteral("看下这段录屏"));
    QCOMPARE(messages.at(0).video.durationSeconds, 7);
}

void TimSdkRemoteIMClientTest::rejectsMissingCredentials() {
    TimSdkRemoteIMClient client(std::make_unique<FakeTimSdkApi>());
    bool ok = true;
    QString error;

    client.connectToService(0, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), [&](bool result, const QString& message) {
        ok = result;
        error = message;
    });

    QVERIFY(!ok);
    QVERIFY(error.contains(QStringLiteral("SDK AppID")));
}

QTEST_MAIN(TimSdkRemoteIMClientTest)
#include "TimSdkRemoteIMClientTest.moc"
