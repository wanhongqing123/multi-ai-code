# Qt Widgets Desktop IM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一个不用 Electron、不用 QML 的 Windows/macOS 专门 IM 应用。

**Architecture:** 新增 `desktop/qt-im` 独立 CMake 工程。Qt Widgets 负责界面，C++ model/storage 负责本地状态，`RemoteIMClient` 接口隔离平台 SDK；先通过 `FakeRemoteIMClient` 做可运行 UI，再分别补 Windows C/C++ SDK adapter 和 macOS Objective-C++ bridge。

**Tech Stack:** Qt 6 Widgets, C++17, CMake, Qt Test, platform-specific Native IM SDK adapters.

---

## File Structure

- Create: `desktop/qt-im/CMakeLists.txt`
- Create: `desktop/qt-im/src/main.cpp`
- Create: `desktop/qt-im/src/model/RemoteIMContact.h`
- Create: `desktop/qt-im/src/model/RemoteIMMessage.h`
- Create: `desktop/qt-im/src/model/ChatState.h`
- Create: `desktop/qt-im/src/model/ChatState.cpp`
- Create: `desktop/qt-im/src/storage/LocalChatHistoryStore.h`
- Create: `desktop/qt-im/src/storage/LocalChatHistoryStore.cpp`
- Create: `desktop/qt-im/src/storage/LocalSettingsStore.h`
- Create: `desktop/qt-im/src/storage/LocalSettingsStore.cpp`
- Create: `desktop/qt-im/src/storage/RemoteIMMediaStore.h`
- Create: `desktop/qt-im/src/storage/RemoteIMMediaStore.cpp`
- Create: `desktop/qt-im/src/im/RemoteIMClient.h`
- Create: `desktop/qt-im/src/im/FakeRemoteIMClient.h`
- Create: `desktop/qt-im/src/im/FakeRemoteIMClient.cpp`
- Create: `desktop/qt-im/src/im/WindowsRemoteIMClient.h`
- Create: `desktop/qt-im/src/im/WindowsRemoteIMClient.cpp`
- Create: `desktop/qt-im/src/im/MacRemoteIMClient.h`
- Create: `desktop/qt-im/src/im/MacRemoteIMClient.mm`
- Create: `desktop/qt-im/src/app/RemoteIMApplication.h`
- Create: `desktop/qt-im/src/app/RemoteIMApplication.cpp`
- Create: `desktop/qt-im/src/ui/MainWindow.h`
- Create: `desktop/qt-im/src/ui/MainWindow.cpp`
- Create: `desktop/qt-im/src/ui/LoginDialog.h`
- Create: `desktop/qt-im/src/ui/LoginDialog.cpp`
- Create: `desktop/qt-im/src/ui/ConversationListWidget.h`
- Create: `desktop/qt-im/src/ui/ConversationListWidget.cpp`
- Create: `desktop/qt-im/src/ui/ChatViewWidget.h`
- Create: `desktop/qt-im/src/ui/ChatViewWidget.cpp`
- Create: `desktop/qt-im/src/ui/MessageBubbleWidget.h`
- Create: `desktop/qt-im/src/ui/MessageBubbleWidget.cpp`
- Create: `desktop/qt-im/src/ui/ImagePreviewDialog.h`
- Create: `desktop/qt-im/src/ui/ImagePreviewDialog.cpp`
- Create: `desktop/qt-im/tests/ChatStateTest.cpp`
- Create: `desktop/qt-im/tests/LocalChatHistoryStoreTest.cpp`
- Create: `desktop/qt-im/tests/FakeRemoteIMClientTest.cpp`

## Task 1: Create Qt Project Skeleton and ChatState Model

**Files:**
- Create: `desktop/qt-im/CMakeLists.txt`
- Create: `desktop/qt-im/src/model/RemoteIMContact.h`
- Create: `desktop/qt-im/src/model/RemoteIMMessage.h`
- Create: `desktop/qt-im/src/model/ChatState.h`
- Create: `desktop/qt-im/src/model/ChatState.cpp`
- Create: `desktop/qt-im/tests/ChatStateTest.cpp`

- [ ] **Step 1: Create CMake skeleton**

Create `desktop/qt-im/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(MultiAIIMDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Test)

add_library(remote_im_core
  src/model/ChatState.cpp
)
target_include_directories(remote_im_core PUBLIC src)
target_link_libraries(remote_im_core PUBLIC Qt6::Core)

add_executable(chat_state_test tests/ChatStateTest.cpp)
target_link_libraries(chat_state_test PRIVATE remote_im_core Qt6::Test)

enable_testing()
add_test(NAME chat_state_test COMMAND chat_state_test)
```

- [ ] **Step 2: Write failing ChatState test**

Create `desktop/qt-im/tests/ChatStateTest.cpp`:

```cpp
#include <QtTest/QtTest>

#include "model/ChatState.h"

class ChatStateTest : public QObject {
    Q_OBJECT

private slots:
    void queuesOutgoingText();
    void receivesIncomingImage();
};

void ChatStateTest::queuesOutgoingText() {
    ChatState state("mac-user");
    state.upsertContact(RemoteIMContact{"android-user", "android-user"});
    state.selectPeer("android-user");

    const RemoteIMMessage message = state.queueOutgoingText("ping");

    QCOMPARE(message.toUserId, QString("android-user"));
    QCOMPARE(message.text, QString("ping"));
    QCOMPARE(message.direction, RemoteIMMessageDirection::Outgoing);
    QCOMPARE(message.status, RemoteIMMessageStatus::Pending);
    QCOMPARE(state.messagesWith("android-user").size(), 1);
}

void ChatStateTest::receivesIncomingImage() {
    ChatState state("mac-user");

    const RemoteIMMessage message = state.receiveImage("android-user", "/tmp/a.jpg", 640, 480, 1024);

    QCOMPARE(message.fromUserId, QString("android-user"));
    QCOMPARE(message.direction, RemoteIMMessageDirection::Incoming);
    QCOMPARE(message.status, RemoteIMMessageStatus::Received);
    QCOMPARE(state.contacts().size(), 1);
}

QTEST_MAIN(ChatStateTest)
#include "ChatStateTest.moc"
```

- [ ] **Step 3: Run test and verify failure**

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build --target chat_state_test
ctest --test-dir desktop/qt-im/build --output-on-failure
```

Expected: configure or compile fails because model files do not exist.

- [ ] **Step 4: Add model headers**

Create `RemoteIMContact.h`:

```cpp
#pragma once

#include <QString>

struct RemoteIMContact {
    QString userId;
    QString displayName;
};
```

Create `RemoteIMMessage.h`:

```cpp
#pragma once

#include <QDateTime>
#include <QString>
#include <QUuid>

enum class RemoteIMMessageDirection {
    Incoming,
    Outgoing
};

enum class RemoteIMMessageStatus {
    Pending,
    Sent,
    Received,
    Failed
};

struct RemoteIMImageAttachment {
    QString localPath;
    int width = 0;
    int height = 0;
    qint64 sizeBytes = 0;
};

struct RemoteIMVoiceAttachment {
    QString localPath;
    int durationSeconds = 1;
};

struct RemoteIMMessage {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString fromUserId;
    QString toUserId;
    QString text;
    RemoteIMMessageDirection direction = RemoteIMMessageDirection::Incoming;
    RemoteIMMessageStatus status = RemoteIMMessageStatus::Received;
    qint64 createdAtMillis = QDateTime::currentMSecsSinceEpoch();
    RemoteIMImageAttachment image;
    RemoteIMVoiceAttachment voice;
    bool hasImage = false;
    bool hasVoice = false;
};
```

Create `ChatState.h`:

```cpp
#pragma once

#include <QList>
#include <QString>

#include "model/RemoteIMContact.h"
#include "model/RemoteIMMessage.h"

class ChatState {
public:
    explicit ChatState(QString ownerUserId);

    QString ownerUserId() const;
    QString selectedPeerId() const;
    QList<RemoteIMContact> contacts() const;
    QList<RemoteIMMessage> messages() const;

    void upsertContact(const RemoteIMContact& contact);
    void selectPeer(const QString& userId);
    RemoteIMMessage queueOutgoingText(const QString& text);
    RemoteIMMessage queueOutgoingImage(const QString& localPath, int width, int height, qint64 sizeBytes);
    RemoteIMMessage queueOutgoingVoice(const QString& localPath, int durationSeconds);
    RemoteIMMessage receiveText(const QString& fromUserId, const QString& text);
    RemoteIMMessage receiveImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    RemoteIMMessage receiveVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    QList<RemoteIMMessage> messagesWith(const QString& peerId) const;
    bool updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status);

private:
    QString requireSelectedPeer() const;
    static QString clean(const QString& value);
    static QString fileName(const QString& path);

    QString ownerUserId_;
    QString selectedPeerId_;
    QList<RemoteIMContact> contacts_;
    QList<RemoteIMMessage> messages_;
};
```

- [ ] **Step 5: Add ChatState implementation**

Create `ChatState.cpp`:

```cpp
#include "model/ChatState.h"

#include <QFileInfo>
#include <stdexcept>

ChatState::ChatState(QString ownerUserId)
    : ownerUserId_(clean(ownerUserId)) {
    if (ownerUserId_.isEmpty()) {
        throw std::invalid_argument("ownerUserId is required");
    }
}

QString ChatState::ownerUserId() const { return ownerUserId_; }
QString ChatState::selectedPeerId() const { return selectedPeerId_; }
QList<RemoteIMContact> ChatState::contacts() const { return contacts_; }
QList<RemoteIMMessage> ChatState::messages() const { return messages_; }

void ChatState::upsertContact(const RemoteIMContact& contact) {
    const QString userId = clean(contact.userId);
    if (userId.isEmpty()) return;
    for (RemoteIMContact& existing : contacts_) {
        if (existing.userId == userId) {
            existing = RemoteIMContact{userId, clean(contact.displayName).isEmpty() ? userId : clean(contact.displayName)};
            return;
        }
    }
    contacts_.append(RemoteIMContact{userId, clean(contact.displayName).isEmpty() ? userId : clean(contact.displayName)});
}

void ChatState::selectPeer(const QString& userId) {
    selectedPeerId_ = clean(userId);
}

RemoteIMMessage ChatState::queueOutgoingText(const QString& text) {
    const QString cleanText = clean(text);
    if (cleanText.isEmpty()) throw std::invalid_argument("text is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = cleanText;
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingImage(const QString& localPath, int width, int height, qint64 sizeBytes) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = "[图片消息] " + fileName(cleanPath);
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.hasImage = true;
    message.image = RemoteIMImageAttachment{cleanPath, width, height, sizeBytes};
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::queueOutgoingVoice(const QString& localPath, int durationSeconds) {
    const QString cleanPath = clean(localPath);
    if (cleanPath.isEmpty()) throw std::invalid_argument("localPath is required");
    RemoteIMMessage message;
    message.fromUserId = ownerUserId_;
    message.toUserId = requireSelectedPeer();
    message.text = QString("[语音消息 %1s]").arg(qMax(1, durationSeconds));
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.hasVoice = true;
    message.voice = RemoteIMVoiceAttachment{cleanPath, qMax(1, durationSeconds)};
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::receiveText(const QString& fromUserId, const QString& text) {
    const QString peerId = clean(fromUserId);
    upsertContact(RemoteIMContact{peerId, peerId});
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = clean(text);
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::receiveImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes) {
    const QString peerId = clean(fromUserId);
    const QString cleanPath = clean(localPath);
    upsertContact(RemoteIMContact{peerId, peerId});
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = "[图片消息] " + fileName(cleanPath);
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.hasImage = true;
    message.image = RemoteIMImageAttachment{cleanPath, width, height, sizeBytes};
    messages_.append(message);
    return message;
}

RemoteIMMessage ChatState::receiveVoice(const QString& fromUserId, const QString& localPath, int durationSeconds) {
    const QString peerId = clean(fromUserId);
    upsertContact(RemoteIMContact{peerId, peerId});
    RemoteIMMessage message;
    message.fromUserId = peerId;
    message.toUserId = ownerUserId_;
    message.text = QString("[语音消息 %1s]").arg(qMax(1, durationSeconds));
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.hasVoice = true;
    message.voice = RemoteIMVoiceAttachment{clean(localPath), qMax(1, durationSeconds)};
    messages_.append(message);
    return message;
}

QList<RemoteIMMessage> ChatState::messagesWith(const QString& peerId) const {
    QList<RemoteIMMessage> result;
    const QString cleanPeerId = clean(peerId);
    for (const RemoteIMMessage& message : messages_) {
        if (message.fromUserId == cleanPeerId || message.toUserId == cleanPeerId) {
            result.append(message);
        }
    }
    return result;
}

bool ChatState::updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status) {
    for (RemoteIMMessage& message : messages_) {
        if (message.id == messageId) {
            message.status = status;
            return true;
        }
    }
    return false;
}

QString ChatState::requireSelectedPeer() const {
    if (selectedPeerId_.isEmpty()) throw std::logic_error("selected peer is required");
    return selectedPeerId_;
}

QString ChatState::clean(const QString& value) {
    return value.trimmed();
}

QString ChatState::fileName(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("image") : name;
}
```

- [ ] **Step 6: Run tests and commit**

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build --target chat_state_test
ctest --test-dir desktop/qt-im/build --output-on-failure
```

Expected: `chat_state_test` passes.

Commit:

```bash
git add desktop/qt-im/CMakeLists.txt desktop/qt-im/src/model desktop/qt-im/tests/ChatStateTest.cpp
git commit -F /tmp/qt-im-task1.msg
```

Use this commit message:

```text
OPTIMIZE: 添加 Qt IM 消息状态模型
EFFECTION: Qt 桌面 IM 具备可测试的本地消息状态
TESTPOINT: Qt ChatState 单测通过
```

## Task 2: Add RemoteIMClient Interface and Fake Client

**Files:**
- Create: `desktop/qt-im/src/im/RemoteIMClient.h`
- Create: `desktop/qt-im/src/im/FakeRemoteIMClient.h`
- Create: `desktop/qt-im/src/im/FakeRemoteIMClient.cpp`
- Create: `desktop/qt-im/tests/FakeRemoteIMClientTest.cpp`
- Modify: `desktop/qt-im/CMakeLists.txt`

- [ ] **Step 1: Write fake client test**

Create `desktop/qt-im/tests/FakeRemoteIMClientTest.cpp`:

```cpp
#include <QtTest/QtTest>

#include "im/FakeRemoteIMClient.h"

class FakeRemoteIMClientTest : public QObject {
    Q_OBJECT

private slots:
    void sendsTextAndEmitsIncomingText();
};

void FakeRemoteIMClientTest::sendsTextAndEmitsIncomingText() {
    FakeRemoteIMClient client;
    QString incomingPeer;
    QString incomingText;

    QObject::connect(&client, &RemoteIMClient::incomingText, [&](const QString& peerId, const QString& text) {
        incomingPeer = peerId;
        incomingText = text;
    });

    bool sent = false;
    client.sendText("android-user", "ping", [&](bool ok, const QString&) {
        sent = ok;
    });
    client.emitIncomingText("android-user", "done");

    QVERIFY(sent);
    QCOMPARE(client.lastTextPeerId(), QString("android-user"));
    QCOMPARE(client.lastText(), QString("ping"));
    QCOMPARE(incomingPeer, QString("android-user"));
    QCOMPARE(incomingText, QString("done"));
}

QTEST_MAIN(FakeRemoteIMClientTest)
#include "FakeRemoteIMClientTest.moc"
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build --target fake_remote_im_client_test
```

Expected: fail because the target and files do not exist.

- [ ] **Step 3: Add RemoteIMClient interface**

Create `desktop/qt-im/src/im/RemoteIMClient.h`:

```cpp
#pragma once

#include <QObject>
#include <QString>
#include <functional>

using RemoteIMCompletion = std::function<void(bool ok, const QString& error)>;

class RemoteIMClient : public QObject {
    Q_OBJECT

public:
    explicit RemoteIMClient(QObject* parent = nullptr) : QObject(parent) {}
    ~RemoteIMClient() override = default;

    virtual void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) = 0;
    virtual void disconnectFromService(RemoteIMCompletion completion) = 0;
    virtual void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) = 0;
    virtual void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) = 0;
    virtual void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) = 0;

signals:
    void incomingText(const QString& fromUserId, const QString& text);
    void incomingImage(const QString& fromUserId, const QString& localPath, int width, int height, qint64 sizeBytes);
    void incomingVoice(const QString& fromUserId, const QString& localPath, int durationSeconds);
    void disconnected();
};
```

- [ ] **Step 4: Add fake client**

Create `FakeRemoteIMClient.h`:

```cpp
#pragma once

#include "im/RemoteIMClient.h"

class FakeRemoteIMClient final : public RemoteIMClient {
    Q_OBJECT

public:
    explicit FakeRemoteIMClient(QObject* parent = nullptr);

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;

    QString lastTextPeerId() const;
    QString lastText() const;
    void failNext(const QString& error);
    void emitIncomingText(const QString& fromUserId, const QString& text);

private:
    void complete(RemoteIMCompletion completion);

    QString lastTextPeerId_;
    QString lastText_;
    QString nextError_;
};
```

Create `FakeRemoteIMClient.cpp`:

```cpp
#include "im/FakeRemoteIMClient.h"

FakeRemoteIMClient::FakeRemoteIMClient(QObject* parent) : RemoteIMClient(parent) {}

void FakeRemoteIMClient::connectToService(int, const QString&, const QString&, RemoteIMCompletion completion) {
    complete(std::move(completion));
}

void FakeRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) {
    lastTextPeerId_ = peerId.trimmed();
    lastText_ = text.trimmed();
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendImage(const QString&, const QString&, RemoteIMCompletion completion) {
    complete(std::move(completion));
}

void FakeRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    complete(std::move(completion));
}

QString FakeRemoteIMClient::lastTextPeerId() const { return lastTextPeerId_; }
QString FakeRemoteIMClient::lastText() const { return lastText_; }
void FakeRemoteIMClient::failNext(const QString& error) { nextError_ = error; }
void FakeRemoteIMClient::emitIncomingText(const QString& fromUserId, const QString& text) {
    emit incomingText(fromUserId, text);
}

void FakeRemoteIMClient::complete(RemoteIMCompletion completion) {
    if (!completion) return;
    if (nextError_.isEmpty()) {
        completion(true, QString());
    } else {
        const QString error = nextError_;
        nextError_.clear();
        completion(false, error);
    }
}
```

- [ ] **Step 5: Update CMake**

Modify `CMakeLists.txt`:

```cmake
add_library(remote_im_core
  src/model/ChatState.cpp
  src/im/FakeRemoteIMClient.cpp
)

add_executable(fake_remote_im_client_test tests/FakeRemoteIMClientTest.cpp)
target_link_libraries(fake_remote_im_client_test PRIVATE remote_im_core Qt6::Test)
add_test(NAME fake_remote_im_client_test COMMAND fake_remote_im_client_test)
```

- [ ] **Step 6: Run tests and commit**

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build
ctest --test-dir desktop/qt-im/build --output-on-failure
```

Expected: `chat_state_test` and `fake_remote_im_client_test` pass.

Commit:

```bash
git add desktop/qt-im/CMakeLists.txt desktop/qt-im/src/im desktop/qt-im/tests/FakeRemoteIMClientTest.cpp
git commit -F /tmp/qt-im-task2.msg
```

Use this commit message:

```text
OPTIMIZE: 抽象 Qt IM 客户端接口
EFFECTION: Qt 桌面 IM 可通过 Fake 客户端开发和测试
TESTPOINT: Qt Fake IM 客户端单测通过
```

## Task 3: Add Storage Layer

**Files:**
- Create: `desktop/qt-im/src/storage/LocalChatHistoryStore.h`
- Create: `desktop/qt-im/src/storage/LocalChatHistoryStore.cpp`
- Create: `desktop/qt-im/src/storage/LocalSettingsStore.h`
- Create: `desktop/qt-im/src/storage/LocalSettingsStore.cpp`
- Create: `desktop/qt-im/src/storage/RemoteIMMediaStore.h`
- Create: `desktop/qt-im/src/storage/RemoteIMMediaStore.cpp`
- Create: `desktop/qt-im/tests/LocalChatHistoryStoreTest.cpp`
- Modify: `desktop/qt-im/CMakeLists.txt`

- [ ] **Step 1: Write history store test**

Create `LocalChatHistoryStoreTest.cpp`:

```cpp
#include <QtTest/QtTest>

#include "storage/LocalChatHistoryStore.h"

class LocalChatHistoryStoreTest : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsMessages();
};

void LocalChatHistoryStoreTest::savesAndLoadsMessages() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalChatHistoryStore store(dir.path());

    ChatState state("mac-user");
    state.upsertContact(RemoteIMContact{"android-user", "android-user"});
    state.selectPeer("android-user");
    state.queueOutgoingText("ping");

    QVERIFY(store.save(state));
    ChatState restored("mac-user");
    QVERIFY(store.load("mac-user", restored));

    QCOMPARE(restored.messagesWith("android-user").size(), 1);
    QCOMPARE(restored.messagesWith("android-user").first().text, QString("ping"));
}

QTEST_MAIN(LocalChatHistoryStoreTest)
#include "LocalChatHistoryStoreTest.moc"
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build --target local_chat_history_store_test
```

Expected: fail because storage files do not exist.

- [ ] **Step 3: Add storage interfaces**

Create `LocalChatHistoryStore.h`:

```cpp
#pragma once

#include <QString>

#include "model/ChatState.h"

class LocalChatHistoryStore {
public:
    explicit LocalChatHistoryStore(QString rootDir);
    bool save(const ChatState& state) const;
    bool load(const QString& ownerUserId, ChatState& state) const;

private:
    QString filePath(const QString& ownerUserId) const;
    QString rootDir_;
};
```

Create `LocalSettingsStore.h`:

```cpp
#pragma once

#include <QString>

struct LocalIMSettings {
    QString userId;
};

class LocalSettingsStore {
public:
    explicit LocalSettingsStore(QString filePath);
    LocalIMSettings load() const;
    bool save(const LocalIMSettings& settings) const;

private:
    QString filePath_;
};
```

Create `RemoteIMMediaStore.h`:

```cpp
#pragma once

#include <QString>

class RemoteIMMediaStore {
public:
    explicit RemoteIMMediaStore(QString rootDir);
    QString imageCachePath(const QString& sourceName) const;
    QString voiceCachePath() const;

private:
    QString ensureDir(const QString& name) const;
    QString rootDir_;
};
```

- [ ] **Step 4: Add storage implementations**

Create `LocalChatHistoryStore.cpp`:

```cpp
#include "storage/LocalChatHistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

LocalChatHistoryStore::LocalChatHistoryStore(QString rootDir) : rootDir_(std::move(rootDir)) {}

bool LocalChatHistoryStore::save(const ChatState& state) const {
    QDir().mkpath(rootDir_);
    QJsonArray messages;
    for (const RemoteIMMessage& message : state.messages()) {
        QJsonObject object;
        object["id"] = message.id;
        object["from"] = message.fromUserId;
        object["to"] = message.toUserId;
        object["text"] = message.text;
        object["direction"] = message.direction == RemoteIMMessageDirection::Outgoing ? "outgoing" : "incoming";
        object["status"] = static_cast<int>(message.status);
        object["createdAtMillis"] = QString::number(message.createdAtMillis);
        object["hasImage"] = message.hasImage;
        object["imagePath"] = message.image.localPath;
        object["imageWidth"] = message.image.width;
        object["imageHeight"] = message.image.height;
        object["imageSizeBytes"] = QString::number(message.image.sizeBytes);
        object["hasVoice"] = message.hasVoice;
        object["voicePath"] = message.voice.localPath;
        object["voiceDurationSeconds"] = message.voice.durationSeconds;
        messages.append(object);
    }
    QFile file(filePath(state.ownerUserId()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(messages).toJson(QJsonDocument::Compact));
    return true;
}

bool LocalChatHistoryStore::load(const QString& ownerUserId, ChatState& state) const {
    QFile file(filePath(ownerUserId));
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) return false;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        if (object["direction"].toString() == "incoming") {
            state.receiveText(object["from"].toString(), object["text"].toString());
        } else {
            state.upsertContact(RemoteIMContact{object["to"].toString(), object["to"].toString()});
            state.selectPeer(object["to"].toString());
            state.queueOutgoingText(object["text"].toString());
        }
    }
    return true;
}

QString LocalChatHistoryStore::filePath(const QString& ownerUserId) const {
    return QDir(rootDir_).filePath(ownerUserId + ".json");
}
```

Create `LocalSettingsStore.cpp` and `RemoteIMMediaStore.cpp` with simple Qt JSON and app-data path helpers:

```cpp
#include "storage/LocalSettingsStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

LocalSettingsStore::LocalSettingsStore(QString filePath) : filePath_(std::move(filePath)) {}

LocalIMSettings LocalSettingsStore::load() const {
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    return LocalIMSettings{object["userId"].toString()};
}

bool LocalSettingsStore::save(const LocalIMSettings& settings) const {
    QFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QJsonObject object;
    object["userId"] = settings.userId.trimmed();
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return true;
}
```

```cpp
#include "storage/RemoteIMMediaStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

RemoteIMMediaStore::RemoteIMMediaStore(QString rootDir) : rootDir_(std::move(rootDir)) {}

QString RemoteIMMediaStore::imageCachePath(const QString& sourceName) const {
    const QString ext = QFileInfo(sourceName).suffix().isEmpty() ? "jpg" : QFileInfo(sourceName).suffix();
    return QDir(ensureDir("images")).filePath(QString("remote-im-image-%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(ext));
}

QString RemoteIMMediaStore::voiceCachePath() const {
    return QDir(ensureDir("voice")).filePath(QString("remote-im-voice-%1.m4a").arg(QDateTime::currentMSecsSinceEpoch()));
}

QString RemoteIMMediaStore::ensureDir(const QString& name) const {
    QDir dir(rootDir_);
    dir.mkpath(name);
    return dir.filePath(name);
}
```

- [ ] **Step 5: Update CMake, run tests, commit**

Add storage `.cpp` files to `remote_im_core`, add `local_chat_history_store_test`, then run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build
ctest --test-dir desktop/qt-im/build --output-on-failure
```

Expected: all Qt tests pass.

Commit:

```bash
git add desktop/qt-im/CMakeLists.txt desktop/qt-im/src/storage desktop/qt-im/tests/LocalChatHistoryStoreTest.cpp
git commit -F /tmp/qt-im-task3.msg
```

Use this commit message:

```text
OPTIMIZE: 添加 Qt IM 本地存储
EFFECTION: Qt 桌面 IM 可保存设置历史和媒体路径
TESTPOINT: Qt 本地历史存储单测通过
```

## Task 4: Add Qt Widgets UI with Fake Client

**Files:**
- Create: `desktop/qt-im/src/main.cpp`
- Create: `desktop/qt-im/src/app/RemoteIMApplication.h`
- Create: `desktop/qt-im/src/app/RemoteIMApplication.cpp`
- Create: `desktop/qt-im/src/ui/MainWindow.h`
- Create: `desktop/qt-im/src/ui/MainWindow.cpp`
- Create: `desktop/qt-im/src/ui/LoginDialog.h`
- Create: `desktop/qt-im/src/ui/LoginDialog.cpp`
- Create: `desktop/qt-im/src/ui/ConversationListWidget.h`
- Create: `desktop/qt-im/src/ui/ConversationListWidget.cpp`
- Create: `desktop/qt-im/src/ui/ChatViewWidget.h`
- Create: `desktop/qt-im/src/ui/ChatViewWidget.cpp`
- Create: `desktop/qt-im/src/ui/MessageBubbleWidget.h`
- Create: `desktop/qt-im/src/ui/MessageBubbleWidget.cpp`
- Create: `desktop/qt-im/src/ui/ImagePreviewDialog.h`
- Create: `desktop/qt-im/src/ui/ImagePreviewDialog.cpp`
- Modify: `desktop/qt-im/CMakeLists.txt`

- [ ] **Step 1: Add app coordinator**

Create `RemoteIMApplication.h`:

```cpp
#pragma once

#include <QObject>
#include <memory>

#include "im/RemoteIMClient.h"
#include "model/ChatState.h"

class RemoteIMApplication : public QObject {
    Q_OBJECT

public:
    RemoteIMApplication(QString ownerUserId, std::unique_ptr<RemoteIMClient> client, QObject* parent = nullptr);
    ChatState& chatState();
    RemoteIMClient& client();
    void addContact(const QString& userId);
    void sendText(const QString& text);
    void sendImage(const QString& path, int width, int height, qint64 sizeBytes);

signals:
    void stateChanged();
    void errorMessage(const QString& message);

private:
    ChatState state_;
    std::unique_ptr<RemoteIMClient> client_;
};
```

Create `RemoteIMApplication.cpp`:

```cpp
#include "app/RemoteIMApplication.h"

RemoteIMApplication::RemoteIMApplication(QString ownerUserId, std::unique_ptr<RemoteIMClient> client, QObject* parent)
    : QObject(parent), state_(ownerUserId), client_(std::move(client)) {
    QObject::connect(client_.get(), &RemoteIMClient::incomingText, this, [this](const QString& fromUserId, const QString& text) {
        state_.receiveText(fromUserId, text);
        emit stateChanged();
    });
}

ChatState& RemoteIMApplication::chatState() { return state_; }
RemoteIMClient& RemoteIMApplication::client() { return *client_; }

void RemoteIMApplication::addContact(const QString& userId) {
    state_.upsertContact(RemoteIMContact{userId.trimmed(), userId.trimmed()});
    state_.selectPeer(userId.trimmed());
    emit stateChanged();
}

void RemoteIMApplication::sendText(const QString& text) {
    RemoteIMMessage message = state_.queueOutgoingText(text);
    emit stateChanged();
    client_->sendText(message.toUserId, message.text, [this, id = message.id](bool ok, const QString& error) {
        state_.updateMessageStatus(id, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error);
        emit stateChanged();
    });
}

void RemoteIMApplication::sendImage(const QString& path, int width, int height, qint64 sizeBytes) {
    RemoteIMMessage message = state_.queueOutgoingImage(path, width, height, sizeBytes);
    emit stateChanged();
    client_->sendImage(message.toUserId, path, [this, id = message.id](bool ok, const QString& error) {
        state_.updateMessageStatus(id, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error);
        emit stateChanged();
    });
}
```

- [ ] **Step 2: Add widgets**

Create a `MainWindow` with a splitter, left list, right chat view, input row, `+` image button, and send button. The constructor should accept `RemoteIMApplication*`.

Core send handler in `MainWindow.cpp`:

```cpp
void MainWindow::sendCurrentText() {
    const QString text = input_->text().trimmed();
    if (text.isEmpty()) return;
    app_->sendText(text);
    input_->clear();
}
```

Core image handler:

```cpp
void MainWindow::pickAndSendImage() {
    const QString path = QFileDialog::getOpenFileName(this, tr("选择图片"), QString(), tr("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty()) return;
    QImageReader reader(path);
    const QSize size = reader.size();
    app_->sendImage(path, qMax(0, size.width()), qMax(0, size.height()), QFileInfo(path).size());
}
```

`ImagePreviewDialog` should use a fullscreen `QDialog` with a black background and a `QLabel` scaled with `Qt::KeepAspectRatio`.

- [ ] **Step 3: Add main executable**

Create `main.cpp`:

```cpp
#include <QApplication>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication remoteIm("desktop-user", std::move(client));
    remoteIm.addContact("android-user");

    MainWindow window(&remoteIm);
    window.resize(980, 680);
    window.show();

    return QApplication::exec();
}
```

- [ ] **Step 4: Update CMake and build app**

Add executable:

```cmake
add_executable(multi_ai_im_desktop
  src/main.cpp
  src/app/RemoteIMApplication.cpp
  src/ui/MainWindow.cpp
  src/ui/LoginDialog.cpp
  src/ui/ConversationListWidget.cpp
  src/ui/ChatViewWidget.cpp
  src/ui/MessageBubbleWidget.cpp
  src/ui/ImagePreviewDialog.cpp
)
target_link_libraries(multi_ai_im_desktop PRIVATE remote_im_core Qt6::Widgets)
```

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build --target multi_ai_im_desktop
```

Expected: app builds and opens with a conversation list and chat area.

- [ ] **Step 5: Commit**

```bash
git add desktop/qt-im/CMakeLists.txt desktop/qt-im/src/main.cpp desktop/qt-im/src/app desktop/qt-im/src/ui
git commit -F /tmp/qt-im-task4.msg
```

Use this commit message:

```text
OPTIMIZE: 添加 Qt Widgets IM 界面
EFFECTION: Win/mac 桌面 IM 具备独立聊天窗口原型
TESTPOINT: Qt 桌面 IM 应用编译并可启动
```

## Task 5: Add Platform SDK Adapter Boundaries

**Files:**
- Create: `desktop/qt-im/src/im/WindowsRemoteIMClient.h`
- Create: `desktop/qt-im/src/im/WindowsRemoteIMClient.cpp`
- Create: `desktop/qt-im/src/im/MacRemoteIMClient.h`
- Create: `desktop/qt-im/src/im/MacRemoteIMClient.mm`
- Modify: `desktop/qt-im/CMakeLists.txt`

- [ ] **Step 1: Add Windows adapter boundary**

Create `WindowsRemoteIMClient.h`:

```cpp
#pragma once

#include "im/RemoteIMClient.h"

class WindowsRemoteIMClient final : public RemoteIMClient {
    Q_OBJECT

public:
    explicit WindowsRemoteIMClient(QObject* parent = nullptr);
    void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;
};
```

Create `WindowsRemoteIMClient.cpp`:

```cpp
#include "im/WindowsRemoteIMClient.h"

WindowsRemoteIMClient::WindowsRemoteIMClient(QObject* parent) : RemoteIMClient(parent) {}

void WindowsRemoteIMClient::connectToService(int, const QString&, const QString&, RemoteIMCompletion completion) {
    completion(false, "Windows IM SDK adapter is not linked in this build");
}

void WindowsRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    completion(true, QString());
}

void WindowsRemoteIMClient::sendText(const QString&, const QString&, RemoteIMCompletion completion) {
    completion(false, "Windows IM SDK adapter is not connected");
}

void WindowsRemoteIMClient::sendImage(const QString&, const QString&, RemoteIMCompletion completion) {
    completion(false, "Windows IM SDK adapter is not connected");
}

void WindowsRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    completion(false, "Windows IM SDK adapter is not connected");
}
```

- [ ] **Step 2: Add macOS adapter boundary**

Create `MacRemoteIMClient.h`:

```cpp
#pragma once

#include "im/RemoteIMClient.h"

class MacRemoteIMClient final : public RemoteIMClient {
    Q_OBJECT

public:
    explicit MacRemoteIMClient(QObject* parent = nullptr);
    void connectToService(int sdkAppId, const QString& userId, const QString& userSig, RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void sendText(const QString& peerId, const QString& text, RemoteIMCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds, RemoteIMCompletion completion) override;
};
```

Create `MacRemoteIMClient.mm`:

```cpp
#include "im/MacRemoteIMClient.h"

MacRemoteIMClient::MacRemoteIMClient(QObject* parent) : RemoteIMClient(parent) {}

void MacRemoteIMClient::connectToService(int, const QString&, const QString&, RemoteIMCompletion completion) {
    completion(false, "macOS IM SDK bridge is not linked in this build");
}

void MacRemoteIMClient::disconnectFromService(RemoteIMCompletion completion) {
    completion(true, QString());
}

void MacRemoteIMClient::sendText(const QString&, const QString&, RemoteIMCompletion completion) {
    completion(false, "macOS IM SDK bridge is not connected");
}

void MacRemoteIMClient::sendImage(const QString&, const QString&, RemoteIMCompletion completion) {
    completion(false, "macOS IM SDK bridge is not connected");
}

void MacRemoteIMClient::sendVoice(const QString&, const QString&, int, RemoteIMCompletion completion) {
    completion(false, "macOS IM SDK bridge is not connected");
}
```

- [ ] **Step 3: Update CMake with platform sources**

Add:

```cmake
if(WIN32)
  target_sources(remote_im_core PRIVATE src/im/WindowsRemoteIMClient.cpp)
endif()

if(APPLE)
  enable_language(OBJCXX)
  target_sources(remote_im_core PRIVATE src/im/MacRemoteIMClient.mm)
endif()
```

- [ ] **Step 4: Build and commit**

Run:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build
```

Expected: macOS build succeeds with the mac adapter boundary compiled; Windows compiles the Windows adapter when run on Windows.

Commit:

```bash
git add desktop/qt-im/CMakeLists.txt desktop/qt-im/src/im/WindowsRemoteIMClient.* desktop/qt-im/src/im/MacRemoteIMClient.*
git commit -F /tmp/qt-im-task5.msg
```

Use this commit message:

```text
OPTIMIZE: 添加 Qt IM 平台 SDK 适配边界
EFFECTION: Win/mac IM SDK 接入点从 UI 和状态层隔离
TESTPOINT: Qt 桌面 IM 平台适配边界编译通过
```

## Task 6: Replace Fake Client with Real Platform Client

**Files:**
- Modify: `desktop/qt-im/src/main.cpp`
- Modify: `desktop/qt-im/src/im/WindowsRemoteIMClient.cpp`
- Modify: `desktop/qt-im/src/im/MacRemoteIMClient.mm`
- Modify: `desktop/qt-im/CMakeLists.txt`

- [ ] **Step 1: Add CMake SDK options**

Add:

```cmake
set(MULTI_AI_IM_WINDOWS_SDK_DIR "" CACHE PATH "Windows IM SDK directory")
set(MULTI_AI_IM_MAC_SDK_DIR "" CACHE PATH "macOS IM SDK directory")

if(WIN32 AND MULTI_AI_IM_WINDOWS_SDK_DIR)
  target_include_directories(remote_im_core PRIVATE "${MULTI_AI_IM_WINDOWS_SDK_DIR}/include")
  target_link_directories(remote_im_core PRIVATE "${MULTI_AI_IM_WINDOWS_SDK_DIR}/lib")
  target_link_libraries(remote_im_core PRIVATE ImSDK)
endif()

if(APPLE AND MULTI_AI_IM_MAC_SDK_DIR)
  target_include_directories(remote_im_core PRIVATE "${MULTI_AI_IM_MAC_SDK_DIR}/include")
  target_link_directories(remote_im_core PRIVATE "${MULTI_AI_IM_MAC_SDK_DIR}")
endif()
```

- [ ] **Step 2: Select platform client in main**

Modify `main.cpp`:

```cpp
#if defined(Q_OS_WIN)
#include "im/WindowsRemoteIMClient.h"
#elif defined(Q_OS_MACOS)
#include "im/MacRemoteIMClient.h"
#else
#include "im/FakeRemoteIMClient.h"
#endif

static std::unique_ptr<RemoteIMClient> createClient() {
#if defined(Q_OS_WIN)
    return std::make_unique<WindowsRemoteIMClient>();
#elif defined(Q_OS_MACOS)
    return std::make_unique<MacRemoteIMClient>();
#else
    return std::make_unique<FakeRemoteIMClient>();
#endif
}
```

Then use:

```cpp
auto client = createClient();
```

- [ ] **Step 3: Implement SDK methods inside adapters**

Keep UI and `RemoteIMApplication` unchanged. Implement adapter methods by mapping SDK callbacks to `RemoteIMCompletion` and emitting:

```cpp
emit incomingText(fromUserId, text);
emit incomingImage(fromUserId, localPath, width, height, sizeBytes);
emit incomingVoice(fromUserId, localPath, durationSeconds);
```

For Windows, keep all SDK includes inside `WindowsRemoteIMClient.cpp`.

For macOS, keep all Objective-C imports inside `MacRemoteIMClient.mm`.

- [ ] **Step 4: Build with SDK path**

macOS command:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build -DMULTI_AI_IM_MAC_SDK_DIR=/absolute/path/to/mac/sdk
cmake --build desktop/qt-im/build --target multi_ai_im_desktop
```

Windows command:

```powershell
cmake -S desktop/qt-im -B desktop/qt-im/build -DMULTI_AI_IM_WINDOWS_SDK_DIR=C:\absolute\path\to\windows\sdk
cmake --build desktop/qt-im/build --target multi_ai_im_desktop
```

Expected: app links with the corresponding platform SDK.

- [ ] **Step 5: Manual smoke test and commit**

Manual expected result:

```text
Desktop app launches
Login connects with built-in SDK configuration and user ID
Desktop -> mobile text works
Desktop -> mobile image works
Mobile -> desktop text works
Mobile -> desktop image works
```

Commit:

```bash
git add desktop/qt-im/CMakeLists.txt desktop/qt-im/src/main.cpp desktop/qt-im/src/im/WindowsRemoteIMClient.cpp desktop/qt-im/src/im/MacRemoteIMClient.mm
git commit -F /tmp/qt-im-task6.msg
```

Use this commit message:

```text
OPTIMIZE: 接入 Qt 桌面 IM 平台 SDK
EFFECTION: Win/mac 专门 IM 应用支持真实 IM 收发
TESTPOINT: Qt 桌面 IM 与移动端文本图片互发验证通过
```

## Final Verification

- [ ] Run Qt tests:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build
cmake --build desktop/qt-im/build
ctest --test-dir desktop/qt-im/build --output-on-failure
```

Expected: all Qt tests pass.

- [ ] Run desktop app with fake client on current platform:

```bash
desktop/qt-im/build/multi_ai_im_desktop
```

Expected: window opens, contact can be selected, text/image bubble appears locally.

- [ ] Run platform SDK smoke test on macOS and Windows:

```text
macOS desktop -> Android/iOS: text and image
Android/iOS -> macOS desktop: text and image
Windows desktop -> Android/iOS: text and image
Android/iOS -> Windows desktop: text and image
```
