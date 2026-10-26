#include <QApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "auth/DesktopAuthClient.h"
#include "im/TimSdkRemoteIMClient.h"
#include "storage/LocalMessageDatabase.h"
#include "ui/MainWindow.h"

namespace {

template <typename Start>
bool awaitBool(Start start, int timeoutMs = 25000) {
    QEventLoop loop;
    struct Result { bool done = false; bool ok = false; };
    auto result = std::make_shared<Result>();
    QPointer<QEventLoop> waiting(&loop);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    start([result, waiting](bool success, const QString&) {
        result->done = true;
        result->ok = success;
        if (waiting) waiting->quit();
    });
    if (!result->done) {
        timer.start(timeoutMs);
        loop.exec();
    }
    return result->done && result->ok;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("MaiChat Group QA"));
    QApplication::setApplicationName(QStringLiteral("Desktop Group Live Probe"));
    const QString credentialPath = qEnvironmentVariable("MAICHAT_GROUP_E2E_ACCOUNTS");
    QFile file(credentialPath);
    if (credentialPath.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "missing isolated test account file\n");
        return 2;
    }
    const auto accounts = QJsonDocument::fromJson(file.readAll()).object()
                              .value(QStringLiteral("accounts")).toArray();
    if (accounts.size() < 3) return 2;
    const auto account = accounts.at(2).toObject();
    const QString accountId = account.value(QStringLiteral("id")).toString();
    const QString password = account.value(QStringLiteral("password")).toString();
    if (accountId.isEmpty() || password.isEmpty()) return 2;

    DesktopAuthClient auth(nullptr, QUrl(QStringLiteral("https://ichat.life/api/v1/")), false);
    if (!awaitBool([&](auto done) { auth.loginWithPassword(accountId, password, done); })) {
        std::fprintf(stderr, "isolated test account login failed\n");
        return 3;
    }
    const auto credential = auth.imCredential();
    if (!credential.isValid()) return 3;
    QTemporaryDir storage;
    if (!storage.isValid()) return 4;
    auto client = std::make_unique<TimSdkRemoteIMClient>();
    RemoteIMApplication im(credential.userId, std::move(client),
        std::make_unique<LocalMessageDatabase>(storage.filePath(QStringLiteral("messages.sqlite"))));
    if (!awaitBool([&](auto done) {
            QObject::connect(&im, &RemoteIMApplication::connectionChanged, &im,
                             [done](bool connected) { done(connected, {}); });
            im.connectToService(credential.sdkAppId, credential.userSig);
        })) {
        std::fprintf(stderr, "Tencent IM login failed\n");
        return 4;
    }

    if (qEnvironmentVariable("MAICHAT_GROUP_E2E_LOGIN_ONLY") == QStringLiteral("1")) {
        QEventLoop wait;
        QTimer::singleShot(2000, &wait, &QEventLoop::quit);
        wait.exec();
        std::printf("im_connected=1\ngroups=%d\n", im.groups().size());
        return 0;
    }

    QString requestedGroup = qEnvironmentVariable("MAICHAT_GROUP_E2E_GROUP_ID");
    if (qEnvironmentVariable("MAICHAT_GROUP_E2E_CREATE_WITH_A") == QStringLiteral("1")) {
        struct FriendResult { bool done = false; QString imId; };
        auto friendResult = std::make_shared<FriendResult>();
        QEventLoop friendWait;
        QPointer<QEventLoop> waiting(&friendWait);
        const QString friendAccountId = accounts.at(0).toObject().value(QStringLiteral("id")).toString();
        auth.fetchFriends([friendResult, waiting, friendAccountId](
            const QList<DesktopFriend>& friends, const QString& error) {
            friendResult->done = error.isEmpty();
            for (const DesktopFriend& friendAccount : friends) {
                if (friendAccount.accountId == friendAccountId)
                    friendResult->imId = friendAccount.imUserId;
            }
            if (waiting) waiting->quit();
        });
        if (!friendResult->done) {
            QTimer::singleShot(15000, &friendWait, &QEventLoop::quit);
            friendWait.exec();
        }
        if (!friendResult->done || friendResult->imId.isEmpty()) {
            std::fprintf(stderr, "test friend A is not available to Mac C\n");
            return 8;
        }
        im.addContact(friendResult->imId, QStringLiteral("QA friend A"));
        struct CreateResult { bool done = false; bool ok = false; RemoteIMGroup group; };
        auto createResult = std::make_shared<CreateResult>();
        QEventLoop createWait;
        QPointer<QEventLoop> createWaiting(&createWait);
        const QString name = QStringLiteral("MaiChat Group QA Desktop %1")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        im.createGroup(name, {friendResult->imId}, [createResult, createWaiting](
            bool ok, const QString&, const RemoteIMGroup& group) {
            createResult->done = true;
            createResult->ok = ok;
            createResult->group = group;
            if (createWaiting) createWaiting->quit();
        });
        if (!createResult->done) {
            QTimer::singleShot(30000, &createWait, &QEventLoop::quit);
            createWait.exec();
        }
        if (!createResult->done || createResult->group.groupId.isEmpty()) {
            std::fprintf(stderr, "Mac C group creation status unknown; check joined groups before retry\n");
            return 8;
        }
        requestedGroup = createResult->group.groupId;
        std::printf("created_group_id=%s\nactivation_ok=%d\n",
                    requestedGroup.toUtf8().constData(), createResult->ok ? 1 : 0);
        std::fflush(stdout);
        if (!createResult->ok) return 8;
    }

    MainWindow window(im);
    window.show();
    QEventLoop groupWait;
    bool found = false;
    QString matchedGroupId;
    QTimer poll;
    poll.setInterval(2500);
    auto locateGroup = [&] {
        for (const auto& group : im.groups()) {
            if (!requestedGroup.isEmpty() && group.groupId != requestedGroup) continue;
            if (requestedGroup.isEmpty() && !group.name.startsWith(QStringLiteral("MaiChat Group QA"))) continue;
            found = true;
            matchedGroupId = group.groupId;
            groupWait.quit();
            break;
        }
        if (!found) im.refreshGroups();
    };
    QObject::connect(&poll, &QTimer::timeout, &groupWait, locateGroup);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &groupWait, &QEventLoop::quit);
    locateGroup();
    if (!found) { poll.start(); deadline.start(60000); groupWait.exec(); }
    if (!found) {
        std::fprintf(stderr, "joined test group not found\n");
        return 5;
    }
    const QString groupId = matchedGroupId;
    const int unreadBeforeOpen = im.groupUnreadCount(groupId);
    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    if (!list) return 5;
    const QString waitLiveText = qEnvironmentVariable("MAICHAT_GROUP_E2E_WAIT_LIVE");
    if (!waitLiveText.isEmpty()) {
        std::printf("ready_for_live=1\n");
        std::fflush(stdout);
        QEventLoop liveWait;
        bool foundLive = false;
        QString remoteId;
        auto inspect = [&] {
            int matches = 0;
            for (const auto& message : im.groupMessages(groupId)) {
                if (message.text != waitLiveText) continue;
                ++matches;
                remoteId = message.id;
            }
            if (matches == 1 && im.groupUnreadCount(groupId) > 0) {
                foundLive = true;
                liveWait.quit();
            }
        };
        const auto connection = QObject::connect(
            &im, &RemoteIMApplication::groupsChanged, &liveWait, inspect);
        QTimer::singleShot(60000, &liveWait, &QEventLoop::quit);
        liveWait.exec();
        QObject::disconnect(connection);
        if (!foundLive) {
            std::fprintf(stderr, "expected live unread group message did not arrive once\n");
            return 5;
        }
        const int beforeOpen = im.groupUnreadCount(groupId);
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() == groupId)
                list->setCurrentRow(row);
        application.processEvents();
        const int afterOpen = im.groupUnreadCount(groupId);
        if (afterOpen != 0) return 5;
        const QString capture = qEnvironmentVariable("MAICHAT_GROUP_E2E_SCREENSHOT");
        if (!capture.isEmpty() && !window.grab().save(capture)) return 7;
        std::printf("group_id=%s\nlive_remote_id=%s\nunread_before_open=%d\nunread_after_open=%d\n",
                    groupId.toUtf8().constData(), remoteId.toUtf8().constData(),
                    beforeOpen, afterOpen);
        return 0;
    }
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toString() == groupId)
            list->setCurrentRow(row);
    }
    application.processEvents();
    if (im.selectedGroupId() != groupId) return 5;
    int memberCount = 0;
    for (const auto& group : im.groups())
        if (group.groupId == groupId) memberCount = group.memberCount;
    const QString expectedIncoming = qEnvironmentVariable("MAICHAT_GROUP_E2E_EXPECT_TEXT");
    if (!expectedIncoming.isEmpty()) {
        QEventLoop historyWait;
        bool received = false;
        auto checkIncoming = [&] {
            int count = 0;
            for (const auto& message : im.groupMessages(groupId))
                if (message.text == expectedIncoming) ++count;
            if (count == 1) { received = true; historyWait.quit(); }
        };
        const auto historyConnection = QObject::connect(
            &im, &RemoteIMApplication::groupsChanged, &historyWait, checkIncoming);
        checkIncoming();
        if (!received) {
            QTimer::singleShot(20000, &historyWait, &QEventLoop::quit);
            historyWait.exec();
        }
        QObject::disconnect(historyConnection);
        if (!received) {
            std::fprintf(stderr, "expected A group history message not found exactly once\n");
            return 5;
        }
    }
    if (memberCount != 3 && qEnvironmentVariable("MAICHAT_GROUP_E2E_EXPECT_MEMBERS") == QStringLiteral("3")) {
        std::fprintf(stderr, "group member count was not 3\n");
        return 5;
    }
    if (qEnvironmentVariable("MAICHAT_GROUP_E2E_READ_ONLY") == QStringLiteral("1")) {
        QString incomingId;
        int incomingCount = 0;
        for (const auto& message : im.groupMessages(groupId)) {
            if (message.text != expectedIncoming) continue;
            ++incomingCount;
            incomingId = message.id;
        }
        const QString capture = qEnvironmentVariable("MAICHAT_GROUP_E2E_SCREENSHOT");
        if (!capture.isEmpty() && !window.grab().save(capture)) return 7;
        std::printf("group_id=%s\nmember_count=%d\nincoming_count=%d\nincoming_remote_id=%s\nunread_before_open=%d\nunread_after_open=%d\n",
                    groupId.toUtf8().constData(), memberCount, incomingCount,
                    incomingId.toUtf8().constData(), unreadBeforeOpen,
                    im.groupUnreadCount(groupId));
        return 0;
    }
    const QString text = qEnvironmentVariable("MAICHAT_GROUP_E2E_SEND_TEXT").isEmpty()
        ? QStringLiteral("Mac C group parity probe %1")
              .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")))
        : qEnvironmentVariable("MAICHAT_GROUP_E2E_SEND_TEXT");
    if (!im.sendGroupText(groupId, text)) return 6;
    QEventLoop sendWait;
    bool sent = false;
    auto sentConnection = QObject::connect(&im, &RemoteIMApplication::groupsChanged, &sendWait, [&] {
        for (const auto& message : im.groupMessages(groupId)) {
            if (message.text == text && !message.pending && !message.failed) {
                sent = true;
                sendWait.quit();
            }
        }
    });
    QTimer::singleShot(20000, &sendWait, &QEventLoop::quit);
    sendWait.exec();
    QObject::disconnect(sentConnection);
    if (!sent) return 6;
    application.processEvents();
    const QString screenshot = qEnvironmentVariable("MAICHAT_GROUP_E2E_SCREENSHOT");
    if (!screenshot.isEmpty() && !window.grab().save(screenshot)) return 7;
    QString incomingRemoteId;
    QString outgoingRemoteId;
    for (const auto& message : im.groupMessages(groupId)) {
        if (message.text == expectedIncoming) incomingRemoteId = message.id;
        if (message.text == text) outgoingRemoteId = message.id;
    }
    std::printf("group_id=%s\nmember_count=%d\nincoming_remote_id=%s\noutgoing_remote_id=%s\nmessage=%s\n",
                groupId.toUtf8().constData(), memberCount,
                incomingRemoteId.toUtf8().constData(), outgoingRemoteId.toUtf8().constData(),
                text.toUtf8().constData());
    return 0;
}
