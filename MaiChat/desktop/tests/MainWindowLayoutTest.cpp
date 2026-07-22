#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStringList>
#include <QStackedWidget>
#include <QInputMethodEvent>
#include <QTest>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

namespace {

void confirmNextContactDeletion() {
    QTimer::singleShot(100, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* messageBox = qobject_cast<QMessageBox*>(widget);
            if (!messageBox || !messageBox->isVisible()) continue;
            if (QAbstractButton* yesButton = messageBox->button(QMessageBox::Yes)) {
                yesButton->click();
            }
            return;
        }
    });
}

}  // namespace

class MainWindowLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void exposesDesktopChatLayoutControls();
    void exposesResizableSplitters();
    void rendersEmptyConversationState();
    void sendsTextFromComposer();
    void returnKeySendsComposerText();
    void commandOrControlReturnInsertsNewlineInComposer();
    void sendsMultilineTextWithoutFlatteningReturns();
    void rendersSentMessageFromTopWithMetadata();
    void rendersRelativeMessageDates();
    void contactsNavigationShowsContactsAndOpensChat();
    void contactsCurrentSelectionDoesNotLeaveContactsPage();
    void settingsNavigationShowsAccountAndSdkDefaults();
    void leftNavigationRailIsResizableAndWider();
    void removesRedundantChromeLabels();
    void conversationListsUseDelegateItemsForSmoothScrolling();
    void rendersMarkdownMessageContent();
    void addContactButtonLivesInNavigationRailOnly();
    void navigationTextIsLeftAlignedAndContactsDoNotShowMessagePreview();
    void sectionTitleFollowsSelectedNavigation();
    void visibleContactsNavigationSwitchesMiddlePane();
    void navigationSelectionFollowsContentStackCurrentPage();
    void contactsDirectoryUsesSingleLineRows();
    void wideChatUsesWiderMessageBubbles();
    void restoredLongMessagesExpandAfterWindowIsShown();
    void slashCommandSuggestionsFillComposer();
    void slashCommandBarLeavesImeCompositionUndisturbed();
    void deleteKeyRemovesContactAndMessagesFromConversationList();
    void deleteKeyRemovesContactAndMessagesFromContactsList();
    void navigationIconsDoNotUsePrivateFontGlyphProperties();
    void conversationListShowsUnreadBadgeAndClearsOnOpen();
};

void MainWindowLayoutTest::exposesDesktopChatLayoutControls() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    QVERIFY(window.findChild<QWidget*>(QStringLiteral("navRail")) != nullptr);
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("conversationPane")) != nullptr);
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("chatContentPane")) != nullptr);
    QVERIFY(window.findChild<QTextEdit*>(QStringLiteral("messageEditor")) != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("sendButton")) != nullptr);
}

void MainWindowLayoutTest::exposesResizableSplitters() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    auto* contentSplitter = window.findChild<QSplitter*>(QStringLiteral("contentSplitter"));
    auto* messageComposerSplitter = window.findChild<QSplitter*>(QStringLiteral("messageComposerSplitter"));
    QVERIFY(contentSplitter != nullptr);
    QVERIFY(messageComposerSplitter != nullptr);
    QCOMPARE(contentSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(messageComposerSplitter->orientation(), Qt::Vertical);
    QVERIFY(contentSplitter->childrenCollapsible() == false);
    QVERIFY(messageComposerSplitter->childrenCollapsible() == false);
}

void MainWindowLayoutTest::rendersEmptyConversationState() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    QVERIFY(window.findChild<QWidget*>(QStringLiteral("emptyMessagesView")) != nullptr);
}

void MainWindowLayoutTest::sendsTextFromComposer() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));

    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(!sendButton->isEnabled());

    editor->setPlainText(QStringLiteral("hello from desktop"));
    QVERIFY(sendButton->isEnabled());
    sendButton->click();

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("hello from desktop"));
    QCOMPARE(editor->toPlainText(), QString());
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
}

void MainWindowLayoutTest::returnKeySendsComposerText() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(editor != nullptr);

    editor->setFocus();
    QTest::keyClicks(editor, "line 1");
    QTest::keyClick(editor, Qt::Key_Return);

    QCOMPARE(fakeClient->lastText(), QStringLiteral("line 1"));
    QCOMPARE(editor->toPlainText(), QString());
}

void MainWindowLayoutTest::commandOrControlReturnInsertsNewlineInComposer() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(editor != nullptr);

    editor->setFocus();
    QTest::keyClicks(editor, "line 1");
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(editor, "line 2");

    QCOMPARE(editor->toPlainText(), QStringLiteral("line 1\nline 2"));
}

void MainWindowLayoutTest::sendsMultilineTextWithoutFlatteningReturns() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));

    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);

    editor->setPlainText(QStringLiteral("line 1\nline 2"));
    sendButton->click();

    QCOMPARE(fakeClient->lastText(), QStringLiteral("line 1\nline 2"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("line 1\nline 2"));

    auto* markdownView = window.findChild<QTextBrowser*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(markdownView != nullptr);
    QVERIFY(markdownView->toPlainText().contains(QStringLiteral("line 1")));
    QVERIFY(markdownView->toPlainText().contains(QStringLiteral("line 2")));
}

void MainWindowLayoutTest::rendersSentMessageFromTopWithMetadata() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    auto* messageLayout = window.findChild<QVBoxLayout*>(QStringLiteral("messageLayout"));

    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(messageLayout != nullptr);

    editor->setPlainText(QStringLiteral("hello from desktop"));
    sendButton->click();

    // 布局 [0] 是「加载更早」按钮行（无更早历史时隐藏），消息行从 [1] 开始。
    QTRY_VERIFY(messageLayout->count() > 1);
    auto* loadEarlierButton = window.findChild<QPushButton*>(QStringLiteral("loadEarlierButton"));
    QVERIFY(loadEarlierButton != nullptr);
    QVERIFY(loadEarlierButton->isHidden());
    QVERIFY(messageLayout->itemAt(1)->widget() != nullptr);
    QCOMPARE(messageLayout->itemAt(1)->widget()->objectName(), QStringLiteral("messageRowOutgoing"));
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("messageBubbleOutgoing")) != nullptr);

    auto* authorLabel = window.findChild<QLabel*>(QStringLiteral("messageAuthorLabel"));
    auto* timeLabel = window.findChild<QLabel*>(QStringLiteral("messageTimeLabel"));
    auto* statusLabel = window.findChild<QLabel*>(QStringLiteral("messageStatusLabel"));
    QVERIFY(authorLabel != nullptr);
    QVERIFY(timeLabel != nullptr);
    QVERIFY(statusLabel != nullptr);
    QCOMPARE(authorLabel->text(), QStringLiteral("desktop-user"));
    QVERIFY(!timeLabel->text().trimmed().isEmpty());
    QCOMPARE(statusLabel->text(), QStringLiteral("✓"));
    QCOMPARE(statusLabel->alignment(), Qt::AlignCenter);
    QCOMPARE(statusLabel->minimumWidth(), 16);
    QCOMPARE(statusLabel->minimumHeight(), 16);
    QVERIFY(statusLabel->styleSheet().contains(QStringLiteral("border: 1px solid #12a150")));
    QVERIFY(statusLabel->styleSheet().contains(QStringLiteral("background: transparent")));
}

void MainWindowLayoutTest::rendersRelativeMessageDates() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    const QDate today = QDate::currentDate();
    const QTime messageTime(16, 13);
    const QDateTime todayTime(today, messageTime, Qt::LocalTime);
    const QDateTime yesterdayTime(today.addDays(-1), messageTime, Qt::LocalTime);
    const QDateTime olderTime(today.addDays(-2), messageTime, Qt::LocalTime);

    RemoteIMMessage todayMessage;
    todayMessage.fromUserId = QStringLiteral("phone-user");
    todayMessage.toUserId = QStringLiteral("desktop-user");
    todayMessage.text = QStringLiteral("today");
    todayMessage.createdAtMillis = todayTime.toMSecsSinceEpoch();

    RemoteIMMessage yesterdayMessage = todayMessage;
    yesterdayMessage.id = QStringLiteral("yesterday-message");
    yesterdayMessage.text = QStringLiteral("yesterday");
    yesterdayMessage.createdAtMillis = yesterdayTime.toMSecsSinceEpoch();

    RemoteIMMessage olderMessage = todayMessage;
    olderMessage.id = QStringLiteral("older-message");
    olderMessage.text = QStringLiteral("older");
    olderMessage.createdAtMillis = olderTime.toMSecsSinceEpoch();

    app.chatState().appendMessageForRestore(todayMessage);
    app.chatState().appendMessageForRestore(yesterdayMessage);
    app.chatState().appendMessageForRestore(olderMessage);

    MainWindow window(app);

    QStringList actualTimes;
    for (const QLabel* label : window.findChildren<QLabel*>(QStringLiteral("messageTimeLabel"))) {
        actualTimes.append(label->text());
    }

    QVERIFY(actualTimes.contains(QStringLiteral("16:13")));
    QVERIFY(actualTimes.contains(QStringLiteral("昨天 16:13")));
    QVERIFY(actualTimes.contains(olderTime.toString(QStringLiteral("M 月 d 日 HH:mm"))));
}

void MainWindowLayoutTest::contactsNavigationShowsContactsAndOpensChat() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(contactsNavButton != nullptr);

    contactsNavButton->click();

    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsPage != nullptr);
    QVERIFY(contentStack != nullptr);
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QVERIFY(contactsList != nullptr);
    QCOMPARE(contactsList->count(), 1);
    QCOMPARE(contactsList->item(0)->data(Qt::UserRole).toString(), QStringLiteral("phone-user"));

    const QRect itemRect = contactsList->visualItemRect(contactsList->item(0));
    QTest::mouseClick(contactsList->viewport(), Qt::LeftButton, Qt::NoModifier, itemRect.center());
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    auto* messagesPage = window.findChild<QWidget*>(QStringLiteral("messagesPage"));
    QVERIFY(messagesPage != nullptr);
    QCOMPARE(contentStack->currentWidget(), messagesPage);
}

void MainWindowLayoutTest::contactsCurrentSelectionDoesNotLeaveContactsPage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user-1"), QStringLiteral("iPhone 1"));
    app.addContact(QStringLiteral("phone-user-2"), QStringLiteral("iPhone 2"));

    MainWindow window(app);
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contactsPage != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();
    QCOMPARE(contentStack->currentWidget(), contactsPage);

    contactsList->setCurrentRow(0);

    QCOMPARE(contentStack->currentWidget(), contactsPage);
}

void MainWindowLayoutTest::settingsNavigationShowsAccountAndSdkDefaults() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* settingsNavButton = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    QVERIFY(settingsNavButton != nullptr);

    settingsNavButton->click();

    auto* settingsPage = window.findChild<QWidget*>(QStringLiteral("settingsPage"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* accountValue = window.findChild<QLabel*>(QStringLiteral("settingsAccountValue"));
    auto* connectionValue = window.findChild<QLabel*>(QStringLiteral("settingsConnectionValue"));
    auto* sdkAppIdValue = window.findChild<QLabel*>(QStringLiteral("settingsSdkAppIdValue"));
    QVERIFY(settingsPage != nullptr);
    QVERIFY(contentStack != nullptr);
    QCOMPARE(contentStack->currentWidget(), settingsPage);
    QVERIFY(accountValue != nullptr);
    QVERIFY(connectionValue != nullptr);
    QVERIFY(sdkAppIdValue != nullptr);
    QCOMPARE(accountValue->text(), QStringLiteral("desktop-user"));
    QCOMPARE(connectionValue->text(), QStringLiteral("未连接"));
    QCOMPARE(sdkAppIdValue->text(), QStringLiteral("1600148979"));
    // 设置页的值应是只读标签而非输入框；导航栏的搜索框是唯一合法的 QLineEdit。
    QVERIFY(settingsPage->findChildren<QLineEdit*>().isEmpty());
}

void MainWindowLayoutTest::leftNavigationRailIsResizableAndWider() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    auto* rootNavigationSplitter = window.findChild<QSplitter*>(QStringLiteral("rootNavigationSplitter"));
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(rootNavigationSplitter != nullptr);
    QVERIFY(navRail != nullptr);
    QVERIFY(contactsNavButton != nullptr);
    QCOMPARE(rootNavigationSplitter->orientation(), Qt::Horizontal);
    QVERIFY(rootNavigationSplitter->childrenCollapsible() == false);
    QCOMPARE(contactsNavButton->text(), QStringLiteral("通讯录"));
    QVERIFY(navRail->minimumWidth() >= 148);
    QVERIFY(navRail->maximumWidth() > navRail->minimumWidth());
    QVERIFY(rootNavigationSplitter->handleWidth() >= 6);
}

void MainWindowLayoutTest::removesRedundantChromeLabels() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    QVERIFY(window.windowTitle().trimmed().isEmpty());
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("accountMark")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("accountLabel")) == nullptr);
}

void MainWindowLayoutTest::conversationListsUseDelegateItemsForSmoothScrolling() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    for (int index = 0; index < 80; ++index) {
        app.addContact(QStringLiteral("phone-user-%1").arg(index), QStringLiteral("iPhone %1").arg(index));
    }

    MainWindow window(app);
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(conversationList != nullptr);
    QVERIFY(conversationList->uniformItemSizes());
    for (int index = 0; index < conversationList->count(); ++index) {
        QVERIFY(conversationList->itemWidget(conversationList->item(index)) == nullptr);
    }

    QVERIFY(contactsNavButton != nullptr);
    contactsNavButton->click();
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsList != nullptr);
    QVERIFY(contactsList->uniformItemSizes());
    for (int index = 0; index < contactsList->count(); ++index) {
        QVERIFY(contactsList->itemWidget(contactsList->item(index)) == nullptr);
    }
}

void MainWindowLayoutTest::rendersMarkdownMessageContent() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    const QString hiddenPrefix = QStringLiteral("\u2063\u200B\u200C\u200D\u2063");
    RemoteIMMessage restoredMessage;
    restoredMessage.fromUserId = QStringLiteral("phone-user");
    restoredMessage.toUserId = QStringLiteral("desktop-user");
    restoredMessage.text = hiddenPrefix
        + QStringLiteral("# Win/Mac 每周 Crash 详细报表\n\n**重点**\n\n- 第一条\n- [链接](https://example.com)");
    restoredMessage.direction = RemoteIMMessageDirection::Incoming;
    app.chatState().appendMessageForRestore(restoredMessage);

    MainWindow window(app);
    auto* markdownView = window.findChild<QTextBrowser*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(markdownView != nullptr);
    QVERIFY(markdownView->toHtml().contains(QStringLiteral("<h1")));
    QVERIFY(!markdownView->toPlainText().contains(QStringLiteral("# Win/Mac")));
    QVERIFY(markdownView->toPlainText().contains(QStringLiteral("重点")));
    QVERIFY(markdownView->toHtml().contains(QStringLiteral("href=\"https://example.com\"")));
}

void MainWindowLayoutTest::addContactButtonLivesInNavigationRailOnly() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    const QList<QPushButton*> addButtons = window.findChildren<QPushButton*>(QStringLiteral("addConversationButton"));

    QVERIFY(navRail != nullptr);
    QCOMPARE(addButtons.size(), 1);
    QCOMPARE(addButtons.first()->parentWidget(), navRail);
}

void MainWindowLayoutTest::navigationTextIsLeftAlignedAndContactsDoNotShowMessagePreview() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("这条消息不应该显示在通讯录"));

    MainWindow window(app);
    QVERIFY(window.styleSheet().contains(QStringLiteral("text-align: left")));

    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(contactsNavButton != nullptr);
    contactsNavButton->click();

    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsList != nullptr);
    QVERIFY(contactsList->count() > 0);
    auto* item = contactsList->item(0);
    QVERIFY(item != nullptr);
    QVERIFY(item->data(Qt::UserRole + 2).toString().isEmpty());
    QVERIFY(item->data(Qt::UserRole + 3).toString().isEmpty());
}

void MainWindowLayoutTest::sectionTitleFollowsSelectedNavigation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("通讯录里不应该显示这条消息摘要"));

    MainWindow window(app);
    const auto labels = window.findChildren<QLabel*>();
    for (QLabel* label : labels) {
        QVERIFY(label->text() != QStringLiteral("远程 IM"));
    }

    auto* messagesTitle = window.findChild<QLabel*>(QStringLiteral("messagesSectionTitle"));
    auto* contactsTitle = window.findChild<QLabel*>(QStringLiteral("contactsSectionTitle"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(messagesTitle != nullptr);
    QVERIFY(contactsTitle != nullptr);
    QCOMPARE(messagesTitle->text(), QStringLiteral("消息"));
    QCOMPARE(contactsTitle->text(), QStringLiteral("通讯录"));
    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(contactsPage != nullptr);
    QVERIFY(conversationList != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QCOMPARE(contactsList->count(), 1);
    QVERIFY(contactsList->item(0)->data(Qt::UserRole + 2).toString().isEmpty());
    QVERIFY(contactsList->item(0)->data(Qt::UserRole + 3).toString().isEmpty());
    QVERIFY(conversationList->item(0)->data(Qt::UserRole + 2).toString().contains(QStringLiteral("通讯录里不应该显示")));
}

void MainWindowLayoutTest::visibleContactsNavigationSwitchesMiddlePane() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("消息页摘要"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* messagesPage = window.findChild<QWidget*>(QStringLiteral("messagesPage"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(contactsPage != nullptr);
    QVERIFY(messagesPage != nullptr);
    QVERIFY(contactsList != nullptr);

    QTest::mouseClick(contactsNavButton, Qt::LeftButton);

    QTRY_COMPARE(contentStack->currentWidget(), contactsPage);
    QVERIFY(!messagesPage->isVisible());
    QVERIFY(contactsPage->isVisible());
    QCOMPARE(contactsList->count(), 1);
    QVERIFY(contactsList->item(0)->data(Qt::UserRole + 2).toString().isEmpty());
}

void MainWindowLayoutTest::navigationSelectionFollowsContentStackCurrentPage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    auto* messageNavButton = window.findChild<QPushButton*>(QStringLiteral("messagesNavButton"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* messagesPage = window.findChild<QWidget*>(QStringLiteral("messagesPage"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));

    QVERIFY(messageNavButton != nullptr);
    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(messagesPage != nullptr);
    QVERIFY(contactsPage != nullptr);

    contactsNavButton->click();
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QCOMPARE(contactsNavButton->property("selected").toBool(), true);

    contentStack->setCurrentWidget(messagesPage);

    QCOMPARE(messageNavButton->property("selected").toBool(), true);
    QCOMPARE(contactsNavButton->property("selected").toBool(), false);
}

void MainWindowLayoutTest::contactsDirectoryUsesSingleLineRows() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("house-obs-studio"), QStringLiteral("house-obs-studio"));

    MainWindow window(app);
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();

    QCOMPARE(contactsList->count(), 1);
    QVERIFY(contactsList->item(0)->sizeHint().height() <= 56);
    QVERIFY(contactsList->sizeHintForRow(0) <= 56);
}

void MainWindowLayoutTest::wideChatUsesWiderMessageBubbles() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1680, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    fakeClient->emitIncomingText(QStringLiteral("phone-user"),
                                 QStringLiteral("这是一段比较长的 AICLI 输出内容，用来验证桌面宽屏下消息气泡不会过窄，"
                                                "否则右侧会出现大片没有意义的空白，文本也会被迫换成太多行。"));

    auto* incomingBubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    QVERIFY(incomingBubble != nullptr);
    QTRY_VERIFY2(incomingBubble->maximumWidth() >= 880,
                 qPrintable(QStringLiteral("max=%1 min=%2")
                                .arg(incomingBubble->maximumWidth())
                                .arg(incomingBubble->minimumWidth())));
    QTRY_VERIFY(incomingBubble->minimumWidth() >= 820);
}

void MainWindowLayoutTest::restoredLongMessagesExpandAfterWindowIsShown() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"),
                                QStringLiteral("这是一段在窗口显示前就已经存在的历史长消息，用来模拟登录后拉取到的 AICLI 输出。"
                                               "窗口完成布局后，这类历史消息也应该使用宽屏消息区的可用宽度，不能继续保持窄气泡。"));

    MainWindow window(app);
    window.resize(1680, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* incomingBubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    QVERIFY(incomingBubble != nullptr);
    auto* messageScroll = window.findChild<QScrollArea*>(QStringLiteral("messageScroll"));
    QVERIFY(messageScroll != nullptr);
    QTRY_VERIFY2(incomingBubble->minimumWidth() >= 820,
                 qPrintable(QStringLiteral("min=%1 max=%2 viewport=%3 expanded=%4")
                                .arg(incomingBubble->minimumWidth())
                                .arg(incomingBubble->maximumWidth())
                                .arg(messageScroll->viewport()->width())
                                .arg(incomingBubble->property("expandedTextBubble").toBool())));
}

void MainWindowLayoutTest::slashCommandSuggestionsFillComposer() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* commandBar = window.findChild<QWidget*>(QStringLiteral("slashCommandBar"));

    QVERIFY(editor != nullptr);
    QVERIFY(commandBar != nullptr);
    QVERIFY(!commandBar->isVisible());

    // 命令栏重建被刻意延后到事件循环下一轮：若在按键事件派发内同步增删控件、隐藏/抬升
    // 悬浮层，会吞掉紧随其后的 KeyRelease，令 Windows 认为按键仍按住而狂发自动重复
    //（输入 /g 变成一长串 g）。因此每次改动输入框后放行一次事件循环，等 0ms 单次定时器
    // 触发、命令栏完成重建，再做同步断言。
    auto typeQuery = [&](const QString& text) {
        editor->setPlainText(text);
        QTest::qWait(200);  // 命令栏重建有 150ms 防抖，等它触发再断言
    };

    typeQuery(QStringLiteral("/st"));
    QVERIFY(commandBar->isVisible());

    const QStringList expectedCommandObjectNames = {
        QStringLiteral("slashCommandButton_status"),
        QStringLiteral("slashCommandButton_plan"),
        QStringLiteral("slashCommandButton_build"),
        QStringLiteral("slashCommandButton_models"),
        QStringLiteral("slashCommandButton_model"),
        QStringLiteral("slashCommandButton_goal"),
        QStringLiteral("slashCommandButton_btw"),
        QStringLiteral("slashCommandButton_diff"),
        QStringLiteral("slashCommandButton_interrupt"),
        QStringLiteral("slashCommandButton_compact"),
        QStringLiteral("slashCommandButton_clear"),
        QStringLiteral("slashCommandButton_help"),
    };

    typeQuery(QStringLiteral("/"));
    QVERIFY(commandBar->isVisible());
    for (const QString& objectName : expectedCommandObjectNames) {
        QVERIFY2(window.findChild<QPushButton*>(objectName) != nullptr, qPrintable(objectName));
    }

    auto* statusButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_status"));
    QVERIFY(statusButton != nullptr);
    statusButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/status"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/pl"));
    QVERIFY(commandBar->isVisible());

    auto* planButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_plan"));
    QVERIFY(planButton != nullptr);
    planButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/plan"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/bu"));
    QVERIFY(commandBar->isVisible());

    auto* buildButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_build"));
    QVERIFY(buildButton != nullptr);
    buildButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/build"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/mo"));
    QVERIFY(commandBar->isVisible());

    auto* modelsButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_models"));
    QVERIFY(modelsButton != nullptr);
    modelsButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/models"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/mod"));
    QVERIFY(commandBar->isVisible());

    auto* modelButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_model"));
    QVERIFY(modelButton != nullptr);
    modelButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/model "));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/go"));
    QVERIFY(commandBar->isVisible());

    auto* goalButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_goal"));
    QVERIFY(goalButton != nullptr);
    goalButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/goal "));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/bt"));
    QVERIFY(commandBar->isVisible());

    auto* btwButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_btw"));
    QVERIFY(btwButton != nullptr);
    btwButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/btw "));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/di"));
    QVERIFY(commandBar->isVisible());

    auto* diffButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_diff"));
    QVERIFY(diffButton != nullptr);
    diffButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/diff "));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/in"));
    QVERIFY(commandBar->isVisible());

    auto* interruptButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_interrupt"));
    QVERIFY(interruptButton != nullptr);
    interruptButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/interrupt"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/co"));
    QVERIFY(commandBar->isVisible());

    auto* compactButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_compact"));
    QVERIFY(compactButton != nullptr);
    compactButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/compact"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/cl"));
    QVERIFY(commandBar->isVisible());

    auto* clearButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_clear"));
    QVERIFY(clearButton != nullptr);
    clearButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/clear"));
    QVERIFY(commandBar->isVisible());

    typeQuery(QStringLiteral("/he"));
    QVERIFY(commandBar->isVisible());

    auto* helpButton = window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_help"));
    QVERIFY(helpButton != nullptr);
    helpButton->click();

    QCOMPARE(editor->toPlainText(), QStringLiteral("/help"));
    QVERIFY(commandBar->isVisible());
}

void MainWindowLayoutTest::slashCommandBarLeavesImeCompositionUndisturbed() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* commandBar = window.findChild<QWidget*>(QStringLiteral("slashCommandBar"));
    QVERIFY(editor != nullptr);
    QVERIFY(commandBar != nullptr);
    editor->setFocus();

    // 输入 "/goal " 让命令栏显示（含 /goal 按钮）。setPlainText 会把光标留在开头，
    // 手动移到末尾，模拟真实输入后的光标位置（组词上屏要接在末尾）。
    editor->setPlainText(QStringLiteral("/goal "));
    {
        QTextCursor cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
    }
    QTRY_VERIFY(commandBar->isVisible());
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_goal")) != nullptr);

    // 模拟输入法开始组词（预编辑串 "n"，未上屏）。命令栏不得在组词期间被重建/隐藏，
    // 否则会打断输入法上下文、把首个拼音键漏成普通字符。预编辑不入文档，已提交文本不变。
    {
        QInputMethodEvent ime(QStringLiteral("n"), {});
        QApplication::sendEvent(editor, &ime);
    }
    QTest::qWait(200);  // 若有未取消的防抖重建会在此触发——不应发生
    QVERIFY(commandBar->isVisible());
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("slashCommandButton_goal")) != nullptr);
    QCOMPARE(editor->toPlainText(), QStringLiteral("/goal "));

    // 组词上屏 "你好"：组词结束后命令栏才刷新，"/goal 你好" 不匹配任何命令 → 隐藏。
    {
        QInputMethodEvent ime(QString(), {});
        ime.setCommitString(QStringLiteral("你好"));
        QApplication::sendEvent(editor, &ime);
    }
    QCOMPARE(editor->toPlainText(), QStringLiteral("/goal 你好"));
    QTRY_VERIFY(!commandBar->isVisible());
}

void MainWindowLayoutTest::deleteKeyRemovesContactAndMessagesFromConversationList() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("remove me"));
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(conversationList != nullptr);

    conversationList->setCurrentRow(0);
    conversationList->setFocus();
    QCOMPARE(conversationList->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("phone-user"));
    confirmNextContactDeletion();
    QTest::keyClick(conversationList, Qt::Key_Delete);

    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 0);
    QCOMPARE(app.chatState().contacts().size(), 1);
    QCOMPARE(app.chatState().contacts().first().userId, QStringLiteral("other-user"));
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("other-user"));
}

void MainWindowLayoutTest::deleteKeyRemovesContactAndMessagesFromContactsList() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("remove me"));
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();
    contactsList->setCurrentRow(0);
    contactsList->setFocus();
    QCOMPARE(contactsList->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("phone-user"));
    confirmNextContactDeletion();
    QTest::keyClick(contactsList, Qt::Key_Delete);

    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 0);
    QCOMPARE(app.chatState().contacts().size(), 1);
    QCOMPARE(app.chatState().contacts().first().userId, QStringLiteral("other-user"));
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("other-user"));
}

void MainWindowLayoutTest::navigationIconsDoNotUsePrivateFontGlyphProperties() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    for (const QString& objectName : {QStringLiteral("messagesNavButton"),
                                      QStringLiteral("contactsNavButton"),
                                      QStringLiteral("settingsNavButton")}) {
        auto* button = window.findChild<QPushButton*>(objectName);
        QVERIFY(button != nullptr);
        QVERIFY(!button->property("navGlyph").isValid());
        QVERIFY(!button->icon().isNull());
    }
}

void MainWindowLayoutTest::conversationListShowsUnreadBadgeAndClearsOnOpen() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    // 非选中会话收到两条实时消息 → 红点数 2；选中会话（phone-user）不计。
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("已读消息"));
    app.chatState().receiveText(QStringLiteral("mac-user"), QStringLiteral("未读一"));
    app.chatState().receiveText(QStringLiteral("mac-user"), QStringLiteral("未读二"));

    MainWindow window(app);
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(conversationList != nullptr);
    QCOMPARE(conversationList->count(), 2);

    int phoneRow = -1;
    int macRow = -1;
    for (int row = 0; row < conversationList->count(); ++row) {
        const QString userId = conversationList->item(row)->data(Qt::UserRole).toString();
        if (userId == QStringLiteral("phone-user")) phoneRow = row;
        if (userId == QStringLiteral("mac-user")) macRow = row;
    }
    QVERIFY(phoneRow >= 0);
    QVERIFY(macRow >= 0);
    QCOMPARE(conversationList->item(phoneRow)->data(Qt::UserRole + 4).toInt(), 0);
    QCOMPARE(conversationList->item(macRow)->data(Qt::UserRole + 4).toInt(), 2);

    // 点开该会话（触发 selectPeer + stateChanged 重刷）：红点清零。
    conversationList->setCurrentRow(macRow);
    for (int row = 0; row < conversationList->count(); ++row) {
        const QString userId = conversationList->item(row)->data(Qt::UserRole).toString();
        if (userId == QStringLiteral("mac-user")) macRow = row;
    }
    QCOMPARE(conversationList->item(macRow)->data(Qt::UserRole + 4).toInt(), 0);
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("mac-user")), 0);
}

QTEST_MAIN(MainWindowLayoutTest)
#include "MainWindowLayoutTest.moc"
