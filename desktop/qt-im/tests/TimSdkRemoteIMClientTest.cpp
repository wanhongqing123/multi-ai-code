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

}  // namespace

class TimSdkRemoteIMClientTest : public QObject {
    Q_OBJECT

private slots:
    void connectsThroughSdkAndSendsTextAndImage();
    void deletesFriendAndConversationThroughSdk();
    void fetchesContactsConversationsAndHistoryAfterLogin();
    void emitsIncomingTextAndImageFromSdkMessages();
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
                {QStringLiteral("user_profile_nick_name"), QStringLiteral("iPhone")}
            }}
        }
    }).toJson(QJsonDocument::Compact));
    fake->conversationListPayload = QString::fromUtf8(QJsonDocument(QJsonArray{
        QJsonObject{
            {QStringLiteral("conv_id"), QStringLiteral("phone-user")},
            {QStringLiteral("conv_type"), 1}
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
    const QList<RemoteIMContact> conversationContacts = qvariant_cast<QList<RemoteIMContact>>(contactsSpy.takeFirst().at(0));
    QCOMPARE(conversationContacts.size(), 1);
    QCOMPARE(conversationContacts.first().userId, QStringLiteral("phone-user"));
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
    // 实时消息改经 messagesReceived 通道送出：与漫游同构、携带稳定 SDK 消息 id
    //（<msg_id>#<elem下标>），本地消息库据此去重。
    QSignalSpy messagesSpy(&client, &RemoteIMClient::messagesReceived);

    client.connectToService(123456, QStringLiteral("desktop-user"), QStringLiteral("sig-value"), nullptr);
    fake->emitMessages(QJsonArray{QJsonObject{
        {QStringLiteral("message_is_from_self"), false},
        {QStringLiteral("message_sender"), QStringLiteral("phone-user")},
        {QStringLiteral("message_msg_id"), QStringLiteral("sdk-msg-1")},
        {QStringLiteral("message_server_time"), 1700000000},
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
    QCOMPARE(messages.size(), 2);

    const RemoteIMMessage& text = messages.at(0);
    QCOMPARE(text.id, QStringLiteral("sdk-msg-1#0"));
    QCOMPARE(text.fromUserId, QStringLiteral("phone-user"));
    QCOMPARE(text.toUserId, QStringLiteral("desktop-user"));
    QCOMPARE(text.direction, RemoteIMMessageDirection::Incoming);
    QCOMPARE(text.text, QStringLiteral("hi"));
    QCOMPARE(text.createdAtMillis, Q_INT64_C(1700000000) * 1000);

    const RemoteIMMessage& image = messages.at(1);
    QCOMPARE(image.id, QStringLiteral("sdk-msg-1#1"));
    QVERIFY(image.hasImage);
    QCOMPARE(image.image.localPath, QStringLiteral("/tmp/incoming.png"));
    QCOMPARE(image.image.width, 640);
    QCOMPARE(image.image.height, 480);
    QCOMPARE(image.image.sizeBytes, static_cast<qint64>(128));
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
