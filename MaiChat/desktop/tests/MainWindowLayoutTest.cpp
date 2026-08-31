#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStringList>
#include <QStackedWidget>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/AppMessageDialog.h"
#include "ui/MainWindow.h"
#include "ui/UiZoom.h"

namespace {

// 确认弹窗是模态的，exec() 会挡住测试线程；用定时器在其嵌套事件循环里点掉。
// 删除类确认的主按钮是 appMessageDanger（红色），普通确认是 appMessageConfirm。
void confirmNextContactDeletion() {
    QTimer::singleShot(100, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<AppMessageDialog*>(widget);
            if (dialog == nullptr || !dialog->isVisible()) continue;
            auto* confirm = dialog->findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
            if (confirm == nullptr) {
                confirm = dialog->findChild<QPushButton*>(QStringLiteral("appMessageConfirm"));
            }
            if (confirm != nullptr) confirm->click();
            return;
        }
    });
}

}  // namespace

class MainWindowLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void exposesDesktopChatLayoutControls();
    void composerUsesEmbeddedIconSendAction();
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
    void globalSearchFindsMatchesAcrossConversationsAndJumps();
    void clickingFilteredConversationJumpsToItsSearchHit();
    void globalSearchReportsNoResultWithLoadedScopeHint();
    void conversationListsUseDelegateItemsForSmoothScrolling();
    void contactDirectoryStaysFlatUntilAGroupExists();
    void contactDirectoryShowsGroupsWithUngroupedLast();
    void clickingGroupHeaderCollapsesItsMembers();
    void contactSearchLooksInsideCollapsedGroups();
    void contactSearchHidesGroupsWithoutAnyHit();
    void groupHeaderCountStaysTheGroupSizeWhileSearching();
    void rendersMarkdownMessageContent();
    void rendersApprovalButtonsAndSendsStructuredDecision();
    void restoresSubmittedApprovalStateFromDatabase();
    void authoritativeApprovalResolutionOverridesLateSentDecision();
    void copiesOriginalMarkdownFromMessageContextMenu();
    void addContactButtonSitsBesideTheSearchBox();
    void navigationTextIsLeftAlignedAndContactsDoNotShowMessagePreview();
    void sectionTitleFollowsSelectedNavigation();
    void contactSearchFiltersDirectoryFuzzily();
    void visibleContactsNavigationSwitchesMiddlePane();
    void navigationSelectionFollowsContentStackCurrentPage();
    void contactsDirectoryUsesSingleLineRows();
    void wideChatUsesWiderMessageBubbles();
    void restoredLongMessagesExpandAfterWindowIsShown();
    void slashCommandSuggestionsFillComposer();
    void slashCommandBarLeavesImeCompositionUndisturbed();
    void deleteKeyClearsMessagesButKeepsContactInConversationList();
    void deleteKeyRemovesContactAndMessagesFromContactsList();
    void navigationIconsDoNotUsePrivateFontGlyphProperties();
    void conversationListShowsUnreadBadgeAndClearsOnOpen();
    void everyNavButtonIsStyledConsistently();
    void navLogoUsesAppIconBrandGradient();
    void connectionStatusLivesOnNavAvatarWithoutText();
    void fileBubbleOffersContextMenu();
    void imageBubbleOffersContextMenu();
    void maximizedImageBubbleOpensOnlyOnePreview();
    void copyAttachmentToPathCopiesOverwritesAndReportsErrors();
    void ctrlShortcutsZoomWholeUi();
    void settingsPanelBorderIsNotCoveredByRows();
    void droppingFilesIntoComposerAttachesThemInsteadOfPastingPaths();
    void quotedReplyRendersQuoteBlockAboveBody();
    void replyBarShowsTargetAndClearsAfterSending();
    void conversationListPutsNewestMessageFirst();
    void droppingAnImageFileSendsTheOriginalFileAsAnImage();
};

// 模拟一次真实拖放：Qt 的 drop 依赖前面的 dragEnter/dragMove 建立内部状态，
// 只发 QDropEvent 会被直接丢掉。位置取 viewport 中心。
static void dropOnComposer(QTextEdit* editor, QMimeData* mime) {
    QWidget* target = editor->viewport();
    const QPointF pos(target->width() / 2.0, target->height() / 2.0);

    QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDragMoveEvent move(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
}

// 拖一个文件进输入框，必须变成附件。此前 QTextEdit 默认行为把 file:/// URL 当成
// 纯文本插进去，用户看到一行路径、发出去的也是一行路径，附件根本没产生。
namespace {

int highlightedRowCount(const QWidget& window) {
    int highlighted = 0;
    for (QWidget* row : window.findChildren<QWidget*>()) {
        if (row->property("searchHit").toBool()) ++highlighted;
    }
    return highlighted;
}

}  // namespace

// 引用回复必须在气泡里真的画出来。
//
// 这条测的是**渲染路径**，不是摘要那几个纯函数——纯函数全绿也可能一个像素都没画上去：
// 之前我改过两处只落地了一处，编译通过、单测通过，是靠把界面跑起来才发现的。
// 「回复」的交互状态：提示条要显示回复对象，发出去之后必须自动清掉。
//
// 不清掉的后果很隐蔽：用户回复完一条，接着发一句无关的话，那句话会莫名其妙
// 也带着上一条的引用——而且发送方自己的界面上看着是对的，只有对端能看出不对。
// 会话列表必须把「刚来消息的人」排到最前面。
//
// 此前它沿用联系人列表的顺序，而那是按 userId 的字母序排的：
// 刚回你消息的人可能排在第 11 位，顶上却是几周没说过话的。
void MainWindowLayoutTest::conversationListPutsNewestMessageFirst() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    // 故意让字母序和时间序相反：alpha 名字最小但消息最旧。
    app.addContact(QStringLiteral("alpha"), QStringLiteral("Alpha"));
    app.addContact(QStringLiteral("bravo"), QStringLiteral("Bravo"));
    app.addContact(QStringLiteral("charlie"), QStringLiteral("Charlie"));
    // 从没聊过的联系人，应当沉到最后。
    app.addContact(QStringLiteral("delta"), QStringLiteral("Delta"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    app.selectPeer(QStringLiteral("alpha"));
    app.sendText(QStringLiteral("最早"));
    app.selectPeer(QStringLiteral("bravo"));
    app.sendText(QStringLiteral("中间"));
    app.selectPeer(QStringLiteral("charlie"));
    app.sendText(QStringLiteral("最新"));
    QCoreApplication::processEvents();

    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    QStringList order;
    for (int row = 0; row < list->count(); ++row) {
        // Qt::UserRole 就是 MainWindow.cpp 里的 UserIdRole（存 userId）。
        order.append(list->item(row)->data(Qt::UserRole).toString());
    }
    QVERIFY2(order.size() >= 4, qPrintable(QStringLiteral("会话数不对：%1").arg(order.size())));
    QCOMPARE(order.at(0), QStringLiteral("charlie"));
    QCOMPARE(order.at(1), QStringLiteral("bravo"));
    QCOMPARE(order.at(2), QStringLiteral("alpha"));
    // 没有消息的排最后，而不是因为名字靠前就冒到上面。
    QCOMPARE(order.at(3), QStringLiteral("delta"));

    // 给最旧的那个补一条新消息，它必须立刻升到第一。
    app.selectPeer(QStringLiteral("alpha"));
    app.sendText(QStringLiteral("又说话了"));
    QCoreApplication::processEvents();
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("alpha"));
}

void MainWindowLayoutTest::replyBarShowsTargetAndClearsAfterSending() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    app.selectPeer(QStringLiteral("phone-user"));

    app.sendText(QStringLiteral("原始消息内容"));
    QCoreApplication::processEvents();

    auto* replyBar = window.findChild<QWidget*>(QStringLiteral("pendingReplyBar"));
    auto* replyLabel = window.findChild<QLabel*>(QStringLiteral("pendingReplyLabel"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(replyBar != nullptr);
    QVERIFY(replyLabel != nullptr);
    QVERIFY(editor != nullptr);
    // 还没点「回复」时提示条不该出现。
    QVERIFY(replyBar->isHidden());

    // 直接调菜单动作背后的入口，而不是模拟右键弹窗：QMenu::exec 会阻塞事件循环，
    // 在测试里点它需要额外的定时器配合，测的还是 Qt 自己的菜单而不是我们的逻辑。
    window.beginReplyTo(app.chatState().messages().last());
    QCoreApplication::processEvents();
    QVERIFY2(!replyBar->isHidden(), "点了回复之后提示条必须出现");
    QVERIFY2(replyLabel->text().contains(QStringLiteral("原始消息内容")),
             qPrintable(QStringLiteral("提示条没显示回复对象：") + replyLabel->text()));

    editor->setPlainText(QStringLiteral("这是回复内容"));
    // 点真实的发送按钮，走用户实际走的那条路。
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(sendButton != nullptr);
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    QVERIFY2(replyBar->isHidden(), "发送后提示条必须收起，否则下一条会带上旧引用");
    const RemoteIMMessage sent = app.chatState().messages().last();
    QCOMPARE(sent.text, QStringLiteral("这是回复内容"));
    QVERIFY2(sent.hasQuote, "回复必须带上引用");
    QVERIFY(sent.quote.digest.contains(QStringLiteral("原始消息内容")));

    // 再发一条普通消息，绝不能继续带引用。
    editor->setPlainText(QStringLiteral("一句无关的话"));
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    const RemoteIMMessage plain = app.chatState().messages().last();
    QCOMPARE(plain.text, QStringLiteral("一句无关的话"));
    QVERIFY2(!plain.hasQuote, "引用是单条回复的属性，不能粘在后续消息上");
}

void MainWindowLayoutTest::quotedReplyRendersQuoteBlockAboveBody() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    app.selectPeer(QStringLiteral("phone-user"));

    app.sendText(QStringLiteral("这是被引用的原始消息"));
    QCoreApplication::processEvents();
    // 没有引用时不该凭空冒出引用块。
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("messageQuoteBlock")) == nullptr);

    RemoteIMQuote quote;
    quote.senderId = QStringLiteral("phone-user");
    quote.digest = QStringLiteral("这是被引用的原始消息");
    quote.kind = QStringLiteral("text");
    app.sendText(QStringLiteral("这是回复"), quote, true);
    QCoreApplication::processEvents();

    QWidget* block = window.findChild<QWidget*>(QStringLiteral("messageQuoteBlock"));
    QVERIFY2(block != nullptr, "带引用的消息必须渲染出引用块");
    // findChild 找得到 ≠ 真的画出来了：部件只要 parent 还在就找得到，
    // 哪怕它压根没被加进布局（deleteLater 也不会立刻销毁它）。
    // 所以断言必须落在「进了父气泡的布局」上，否则这条测试是恒真的。
    QWidget* bubble = block->parentWidget();
    QVERIFY(bubble != nullptr);
    QVERIFY(bubble->layout() != nullptr);
    QVERIFY2(bubble->layout()->indexOf(block) >= 0, "引用块必须被加入气泡布局");
    // 并且排在正文之上。
    QCOMPARE(bubble->layout()->indexOf(block), 0);
    auto* text = block->findChild<QLabel*>(QStringLiteral("messageQuoteText"));
    QVERIFY(text != nullptr);
    QVERIFY2(text->text().contains(QStringLiteral("这是被引用的原始消息")),
             qPrintable(QStringLiteral("引用块内容不对：") + text->text()));
    QVERIFY(text->text().contains(QStringLiteral("phone-user")));
}

void MainWindowLayoutTest::droppingFilesIntoComposerAttachesThemInsteadOfPastingPaths() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString docPath = QDir(dir.path()).filePath(QStringLiteral("report.txt"));
    QFile doc(docPath);
    QVERIFY(doc.open(QIODevice::WriteOnly));
    doc.write("hello");
    doc.close();

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(docPath)});
    dropOnComposer(editor, &mime);

    // 输入框里不能出现路径文本，只能有一枚内联对象（U+FFFC）。
    QVERIFY(!editor->toPlainText().contains(QStringLiteral("file://")));
    QVERIFY(!editor->toPlainText().contains(QStringLiteral("report.txt")));
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(sendButton->isEnabled());

    sendButton->click();
    QCOMPARE(fakeClient->lastFilePeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastFilePath(), docPath);
    QCOMPARE(fakeClient->lastFileName(), QStringLiteral("report.txt"));
}

// 拖进来的图片按图片发，且发的是原文件本身——不重新编码成 PNG，
// 否则一张 3MB 的 JPG 会被放大成十几 MB。
void MainWindowLayoutTest::droppingAnImageFileSendsTheOriginalFileAsAnImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = QDir(dir.path()).filePath(QStringLiteral("photo.png"));
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath, "PNG"));

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(imagePath)});
    dropOnComposer(editor, &mime);

    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    sendButton->click();

    QCOMPARE(fakeClient->lastImagePeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastImagePath(), imagePath);  // 原文件，不是临时 PNG 副本
    QCOMPARE(fakeClient->lastFilePath(), QString());   // 没有走文件卡分支
}

void MainWindowLayoutTest::settingsPanelBorderIsNotCoveredByRows() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    auto* panel = window.findChild<QWidget*>(QStringLiteral("settingsPanel"));
    QVERIFY(panel != nullptr);
    panel->resize(600, 400);
    const QImage painted = panel->grab().toImage();
    QVERIFY(!painted.isNull());

    // 子控件不得覆盖面板外框。这里断言的是"画出来了什么"：
    // 沿高度多点采样，左右边框都得在。
    const auto nearBorder = [](QRgb c) {
        // 抗锯齿会让边框像素略有出入，允许小幅偏差但必须明显不是白色。
        return qAbs(qRed(c) - 0xda) < 24 && qAbs(qGreen(c) - 0xe4) < 24
               && qAbs(qBlue(c) - 0xf0) < 24;
    };

    int leftHits = 0;
    int rightHits = 0;
    int sampled = 0;
    QStringList leftMisses;
    QStringList rightMisses;
    const auto describeMiss = [](int y, QRgb first, QRgb second) {
        return QStringLiteral("y=%1 [%2,%3]")
            .arg(y)
            .arg(QString::number(first & 0x00ffffff, 16).rightJustified(6, QLatin1Char('0')))
            .arg(QString::number(second & 0x00ffffff, 16).rightJustified(6, QLatin1Char('0')));
    };
    // 圆角边界会产生平台相关的抗锯齿像素，竖边检查应避开四个圆角。
    const int cornerGuard = UiZoom::s(12);
    for (int y = cornerGuard; y < painted.height() - cornerGuard; y += UiZoom::s(12)) {
        ++sampled;
        const QRgb leftOuter = painted.pixel(0, y);
        const QRgb leftInner = painted.pixel(1, y);
        if (nearBorder(leftOuter) || nearBorder(leftInner)) {
            ++leftHits;
        } else {
            leftMisses.append(describeMiss(y, leftOuter, leftInner));
        }
        const int w = painted.width();
        const QRgb rightOuter = painted.pixel(w - 1, y);
        const QRgb rightInner = painted.pixel(w - 2, y);
        if (nearBorder(rightOuter) || nearBorder(rightInner)) {
            ++rightHits;
        } else {
            rightMisses.append(describeMiss(y, rightOuter, rightInner));
        }
    }
    QVERIFY(sampled > 5);
    QVERIFY2(leftHits == sampled,
             qPrintable(QStringLiteral("左边框只在 %1/%2 个采样点上存在，面板 %3x%4，缺失：%5")
                            .arg(leftHits)
                            .arg(sampled)
                            .arg(painted.width())
                            .arg(painted.height())
                            .arg(leftMisses.join(QStringLiteral("; ")))));
    QVERIFY2(rightHits == sampled,
             qPrintable(QStringLiteral("右边框只在 %1/%2 个采样点上存在，面板 %3x%4，缺失：%5")
                            .arg(rightHits)
                            .arg(sampled)
                            .arg(painted.width())
                            .arg(painted.height())
                            .arg(rightMisses.join(QStringLiteral("; ")))));
}

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

void MainWindowLayoutTest::composerUsesEmbeddedIconSendAction() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();

    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    QTRY_VERIFY(editor->width() > sendButton->width());

    QCOMPARE(sendButton->parentWidget(), editor);
    QCOMPARE(sendButton->text(), QString());
    QVERIFY(!sendButton->icon().isNull());
    QCOMPARE(sendButton->toolTip(), QStringLiteral("发送消息"));
    QVERIFY(editor->rect().contains(sendButton->geometry().topLeft()));
    QVERIFY(editor->rect().contains(sendButton->geometry().bottomRight()));

    const int rightInset = editor->width() - sendButton->geometry().right() - 1;
    const int bottomInset = editor->height() - sendButton->geometry().bottom() - 1;
    QVERIFY(rightInset > 0);
    QVERIFY(bottomInset > 0);
    QVERIFY(rightInset <= sendButton->width() / 2);
    QVERIFY(bottomInset <= sendButton->height() / 2);

    const QList<QPushButton*> buttons = window.findChildren<QPushButton*>();
    for (const QPushButton* button : buttons) {
        QVERIFY(button->toolTip() != QStringLiteral("语音消息"));
    }
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
    auto* avatarLabel = window.findChild<QLabel*>(QStringLiteral("messageAvatarOutgoing"));
    QVERIFY(authorLabel != nullptr);
    QVERIFY(timeLabel != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(avatarLabel != nullptr);
    QCOMPARE(authorLabel->text(), QStringLiteral("desktop-user"));
    QCOMPARE(avatarLabel->text(), QStringLiteral("D"));
    QCOMPARE(avatarLabel->property("avatarUserId").toString(), QStringLiteral("desktop-user"));
    QCOMPARE(avatarLabel->minimumSize(), QSize(40, 40));
    QCOMPARE(avatarLabel->maximumSize(), QSize(40, 40));
    QVERIFY(!timeLabel->text().trimmed().isEmpty());
    QCOMPARE(statusLabel->text(), QStringLiteral("✓"));
    QCOMPARE(statusLabel->alignment(), Qt::AlignCenter);
    QCOMPARE(statusLabel->minimumWidth(), 16);
    QCOMPARE(statusLabel->minimumHeight(), 16);
    QVERIFY(statusLabel->styleSheet().contains(QStringLiteral("border: 1px solid #12a150")));
    QVERIFY(statusLabel->styleSheet().contains(QStringLiteral("background: transparent")));
    // 状态图标要和气泡平级挂在消息行上，不能落进气泡内部。
    // 必须从 statusLabel 反查所在行，不能用 window.findChild 直接找气泡：pending→sent
    // 是「建新行 + 旧行 deleteLater + 插入新行」，旧 pending 行在事件循环真正销毁前仍留在
    // 对象树里，findChild 可能拿到那条旧气泡。而 Pending 态不生成状态图标，statusLabel
    // 只可能来自新行——拿旧气泡和新行的图标比祖先关系恒为假，断言会假通过，退回旧实现照样绿。
    QWidget* statusRow = statusLabel->parentWidget();
    QVERIFY(statusRow != nullptr);
    QCOMPARE(statusRow->objectName(), QStringLiteral("messageRowOutgoing"));
    QCOMPARE(statusRow, messageLayout->itemAt(1)->widget());
    // 气泡应当是同一行里的兄弟节点。父子关系已被上面的 parentWidget 断言锁死，
    // 再补一条 isAncestorOf 是恒真的废断言，故不写。
    QVERIFY(statusRow->findChild<QWidget*>(QStringLiteral("messageBubbleOutgoing"),
                                           Qt::FindDirectChildrenOnly) != nullptr);
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
    // 导航按钮按需求只留图标：文字移到 accessibleName / tooltip。这里同时钉住三点——
    // 确实没有文字、仍能认出是哪个入口、图标没漏设（只去文字不给图标就成了空按钮）。
    QVERIFY(contactsNavButton->text().isEmpty());
    QCOMPARE(contactsNavButton->accessibleName(), QStringLiteral("通讯录"));
    QVERIFY(!contactsNavButton->icon().isNull());
    // 纯图标之后导航栏按需求收窄，并且不再可拉伸：min==max 才能保证它不会被
    // splitter 拖回原来那么宽。
    QVERIFY2(navRail->maximumWidth() <= 96, "导航栏应当是窄条，不该再占一整列");
    QCOMPARE(navRail->minimumWidth(), navRail->maximumWidth());
    QVERIFY(rootNavigationSplitter->handleWidth() >= 6);

    // 一列图标的尺寸必须一致：混用尺寸时肉眼看到的就是「有的大有的小」，
    // 而每个按钮单独看都正常，所以要横向比。
    const QList<QPushButton*> navButtons{
        window.findChild<QPushButton*>(QStringLiteral("messagesNavButton")),
        contactsNavButton,
        window.findChild<QPushButton*>(QStringLiteral("remoteNavButton")),
        window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"))};
    for (QPushButton* button : navButtons) {
        QVERIFY(button != nullptr);
        QCOMPARE(button->iconSize(), navButtons.first()->iconSize());
    }

    // 纯图标导航栏里，图标是四个入口唯一的辨识依据，太小就只能靠位置记。
    // 但放大图标最容易越过的界线是「图标撑出按钮被裁掉」，所以两条一起钉：
    // 够大，且仍装得进按钮。
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (QPushButton* button : navButtons) {
        QVERIFY2(button->iconSize().width() >= 24, "导航栏图标应当比会话栏的「+」大一档");
        QVERIFY2(button->iconSize().width() + 8 <= button->width(),
                 "图标放大后必须仍装得进按钮，否则会被裁掉");
        QVERIFY2(button->width() <= navRail->width(), "按钮不能宽过导航栏本身");
    }
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
    app.chatState().upsertContact(RemoteIMContact{
        QStringLiteral("phone-user-0"),
        QStringLiteral("iPhone 0"),
        QStringLiteral("https://example.com/iphone.png")
    });

    MainWindow window(app);
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(conversationList != nullptr);
    QVERIFY(conversationList->uniformItemSizes());
    QCOMPARE(conversationList->item(0)->data(Qt::UserRole + 5).toString(),
             QStringLiteral("https://example.com/iphone.png"));
    for (int index = 0; index < conversationList->count(); ++index) {
        QVERIFY(conversationList->itemWidget(conversationList->item(index)) == nullptr);
    }

    QVERIFY(contactsNavButton != nullptr);
    contactsNavButton->click();
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsList != nullptr);
    // 通讯录有意不等高：分组表头比联系人行矮。开着 uniformItemSizes 会让所有行
    // 都按第一行的高度画，表头和联系人叠在一起。会话列表仍然是等高的。
    QVERIFY(!contactsList->uniformItemSizes());
    QCOMPARE(contactsList->item(0)->data(Qt::UserRole + 5).toString(),
             QStringLiteral("https://example.com/iphone.png"));
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

void MainWindowLayoutTest::rendersApprovalButtonsAndSendsStructuredDecision() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("multi-ai-code"), QStringLiteral("Multi-AI Code"));

    RemoteIMMessage request;
    request.id = QStringLiteral("approval-message-1");
    request.fromUserId = QStringLiteral("multi-ai-code");
    request.toUserId = QStringLiteral("desktop-user");
    request.text = QStringLiteral("Codex 请求执行一条高风险命令");
    request.direction = RemoteIMMessageDirection::Incoming;
    request.status = RemoteIMMessageStatus::Received;
    request.origin = RemoteIMMessageOrigin::Machine;
    request.hasApprovalRequest = true;
    request.approvalRequest = RemoteIMApprovalRequest{
        QStringLiteral("approval-ui-1"),
        {RemoteIMApprovalAction::ApproveOnce,
         RemoteIMApprovalAction::ApprovePrefix,
         RemoteIMApprovalAction::Reject}
    };
    app.chatState().appendMessageForRestore(request);

    MainWindow window(app);
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalApproveOnceButton")) != nullptr);
    auto* prefixButton =
        window.findChild<QPushButton*>(QStringLiteral("approvalApprovePrefixButton"));
    QVERIFY(prefixButton != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalRejectButton")) != nullptr);

    fakeClient->failNext(QStringLiteral("network unavailable"));
    fakeClient->deferNextSend();
    QTest::mouseClick(prefixButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    auto* sendingLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(sendingLabel != nullptr);
    QVERIFY(sendingLabel->text().contains(QStringLiteral("正在发送")));

    // 网络回调到达前切走再切回，会销毁点击时的气泡。失败处理必须依据消息模型
    // 重建当前气泡，不能只操作已经失效的旧控件指针。
    app.addContact(QStringLiteral("other-peer"), QStringLiteral("Other"));
    app.selectPeer(QStringLiteral("multi-ai-code"));
    QCoreApplication::processEvents();
    confirmNextContactDeletion();  // 关闭发送失败提示的模态框，继续检查按钮是否恢复。
    fakeClient->finishDeferredSend();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    prefixButton = window.findChild<QPushButton*>(QStringLiteral("approvalApprovePrefixButton"));
    QVERIFY(prefixButton != nullptr);
    QVERIFY(prefixButton->isVisible());
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("approvalSentLabel")) == nullptr);

    QTest::mouseClick(prefixButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("multi-ai-code"));
    QCOMPARE(fakeClient->lastApprovalToken(), QStringLiteral("approval-ui-1"));
    QCOMPARE(fakeClient->lastApprovalAction(), RemoteIMApprovalAction::ApprovePrefix);
    auto* sentLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(sentLabel != nullptr);
    QVERIFY(sentLabel->text().contains(QStringLiteral("已发送")));

    // 再切走/切回强制重建：已发送状态来自出站决定消息，而不是旧气泡控件。
    app.selectPeer(QStringLiteral("other-peer"));
    app.selectPeer(QStringLiteral("multi-ai-code"));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.findChild<QPushButton*>(
                QStringLiteral("approvalApproveOnceButton")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("approvalSentLabel")) != nullptr);
}

void MainWindowLayoutTest::restoresSubmittedApprovalStateFromDatabase() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("messages.db"));
    {
        LocalMessageDatabase database(databasePath);
        QVERIFY(database.isOpen());
        database.upsertContact(RemoteIMContact{
            QStringLiteral("multi-ai-code"),
            QStringLiteral("Multi-AI Code")
        });

        RemoteIMMessage request;
        request.id = QStringLiteral("approval-restart-request");
        request.fromUserId = QStringLiteral("multi-ai-code");
        request.toUserId = QStringLiteral("desktop-user");
        request.text = QStringLiteral("Codex 请求执行一条高风险命令");
        request.direction = RemoteIMMessageDirection::Incoming;
        request.status = RemoteIMMessageStatus::Received;
        request.origin = RemoteIMMessageOrigin::Machine;
        request.createdAtMillis = 1700000000100LL;
        request.hasApprovalRequest = true;
        request.approvalRequest = RemoteIMApprovalRequest{
            QStringLiteral("approval-restart-1"),
            {RemoteIMApprovalAction::ApproveOnce, RemoteIMApprovalAction::Reject}
        };
        QVERIFY(database.insertMessageIfAbsent(request, QStringLiteral("multi-ai-code")));

        RemoteIMMessage decision;
        decision.id = QStringLiteral("approval-restart-decision");
        decision.fromUserId = QStringLiteral("desktop-user");
        decision.toUserId = QStringLiteral("multi-ai-code");
        decision.text = QStringLiteral("审批操作：同意本次");
        decision.direction = RemoteIMMessageDirection::Outgoing;
        decision.status = RemoteIMMessageStatus::Sent;
        decision.origin = RemoteIMMessageOrigin::Human;
        decision.createdAtMillis = 1700000000200LL;
        decision.hasApprovalDecision = true;
        decision.approvalDecision = RemoteIMApprovalDecision{
            QStringLiteral("approval-restart-1"),
            RemoteIMApprovalAction::ApproveOnce
        };
        QVERIFY(database.insertMessageIfAbsent(decision, QStringLiteral("multi-ai-code")));
    }

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto database = std::make_unique<LocalMessageDatabase>(databasePath);
    QVERIFY(database->isOpen());
    RemoteIMApplication app(
        QStringLiteral("desktop-user"),
        std::move(client),
        std::move(database));
    app.selectPeer(QStringLiteral("multi-ai-code"));

    MainWindow window(app);
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.findChild<QPushButton*>(
                QStringLiteral("approvalApproveOnceButton")) == nullptr);
    auto* sentLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(sentLabel != nullptr);
    QVERIFY(sentLabel->text().contains(QStringLiteral("已发送")));
}

void MainWindowLayoutTest::authoritativeApprovalResolutionOverridesLateSentDecision() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("multi-ai-code"), QStringLiteral("Multi-AI Code"));

    RemoteIMMessage request;
    request.id = QStringLiteral("approval-authoritative-request");
    request.fromUserId = QStringLiteral("multi-ai-code");
    request.toUserId = QStringLiteral("desktop-user");
    request.text = QStringLiteral("需要审批");
    request.direction = RemoteIMMessageDirection::Incoming;
    request.status = RemoteIMMessageStatus::Received;
    request.origin = RemoteIMMessageOrigin::Machine;
    request.hasApprovalRequest = true;
    request.approvalRequest = RemoteIMApprovalRequest{
        QStringLiteral("approval-authoritative-1"),
        {RemoteIMApprovalAction::ApproveOnce, RemoteIMApprovalAction::Reject}
    };
    app.chatState().appendMessageForRestore(request);

    RemoteIMMessage resolution;
    resolution.id = QStringLiteral("approval-authoritative-resolution");
    resolution.fromUserId = QStringLiteral("multi-ai-code");
    resolution.toUserId = QStringLiteral("desktop-user");
    resolution.text = QStringLiteral("该审批收到新的 IM 消息后已自动拒绝");
    resolution.direction = RemoteIMMessageDirection::Incoming;
    resolution.status = RemoteIMMessageStatus::Received;
    resolution.origin = RemoteIMMessageOrigin::Machine;
    resolution.hasApprovalDecision = true;
    resolution.approvalDecision = RemoteIMApprovalDecision{
        QStringLiteral("approval-authoritative-1"),
        RemoteIMApprovalAction::AutoDeclined
    };
    app.chatState().appendMessageForRestore(resolution);

    RemoteIMMessage lateDecision;
    lateDecision.id = QStringLiteral("approval-authoritative-late-decision");
    lateDecision.fromUserId = QStringLiteral("desktop-user");
    lateDecision.toUserId = QStringLiteral("multi-ai-code");
    lateDecision.text = QStringLiteral("审批操作：同意本次");
    lateDecision.direction = RemoteIMMessageDirection::Outgoing;
    lateDecision.status = RemoteIMMessageStatus::Sent;
    lateDecision.origin = RemoteIMMessageOrigin::Human;
    lateDecision.hasApprovalDecision = true;
    lateDecision.approvalDecision = RemoteIMApprovalDecision{
        QStringLiteral("approval-authoritative-1"),
        RemoteIMApprovalAction::ApproveOnce
    };
    app.chatState().appendMessageForRestore(lateDecision);

    MainWindow window(app);
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.findChild<QPushButton*>(
                QStringLiteral("approvalApproveOnceButton")) == nullptr);
    auto* resolvedLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(resolvedLabel != nullptr);
    QVERIFY(resolvedLabel->text().contains(QStringLiteral("自动拒绝")));
}

void MainWindowLayoutTest::copiesOriginalMarkdownFromMessageContextMenu() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    const QString markdown = QStringLiteral(
        "# 原始标题\n\n"
        "**加粗** 与 `code`\n\n"
        "- 列表项\n\n"
        "```cpp\nint answer = 42;\n```\n\n"
        "[链接](https://example.com?a=1&b=2)");
    app.chatState().receiveText(QStringLiteral("phone-user"), markdown);

    MainWindow window(app);
    auto* markdownView = window.findChild<QTextBrowser*>(QStringLiteral("messageMarkdownView"));
    auto* copyOriginalAction = window.findChild<QAction*>(QStringLiteral("copyOriginalDataAction"));
    QVERIFY(markdownView != nullptr);
    QVERIFY(copyOriginalAction != nullptr);
    QCOMPARE(copyOriginalAction->text(), QStringLiteral("复制原始数据"));
    QVERIFY(!markdownView->toPlainText().contains(QStringLiteral("# 原始标题")));
    QVERIFY(!markdownView->toPlainText().contains(QStringLiteral("**加粗**")));

    QApplication::clipboard()->setText(QStringLiteral("旧剪贴板内容"));
    copyOriginalAction->trigger();

    QCOMPARE(QApplication::clipboard()->text(), markdown);
}

void MainWindowLayoutTest::addContactButtonSitsBesideTheSearchBox() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    const QList<QPushButton*> addButtons = window.findChildren<QPushButton*>(QStringLiteral("addConversationButton"));

    QVERIFY(navRail != nullptr);
    // 全窗口只能有一个「添加联系人」：之前它在导航栏，现在移到搜索框旁边，
    // 两处都留着就会出现两个入口。
    QCOMPARE(addButtons.size(), 1);
    QVERIFY2(addButtons.first()->parentWidget() != navRail,
             "加号已从导航栏移出，不该还挂在导航栏上");

    // 和搜索框同一个父级、同一行：这是「放到搜索旁边」的可验证含义。
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    QVERIFY(search != nullptr);
    QCOMPARE(addButtons.first()->parentWidget(), search->parentWidget());
}

void MainWindowLayoutTest::navigationTextIsLeftAlignedAndContactsDoNotShowMessagePreview() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("这条消息不应该显示在通讯录"));

    MainWindow window(app);
    // 原来这里断言样式表含 text-align:left——那是导航按钮还有文字时的要求。
    // 现在是纯图标，改为直接钉住「没有文字」这个事实。
    auto* messagesNavButton = window.findChild<QPushButton*>(QStringLiteral("messagesNavButton"));
    QVERIFY(messagesNavButton != nullptr);
    QVERIFY(messagesNavButton->text().isEmpty());

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

    // 两页的标题文字都已被各自的搜索框取代，所以这里要求它们不存在。
    QVERIFY2(messagesTitle == nullptr, "消息页的标题应当已被搜索行取代");
    QVERIFY2(contactsTitle == nullptr, "通讯录页的标题应当已被搜索框取代");
    for (QLabel* label : labels) {
        QVERIFY2(label->text() != QStringLiteral("通讯录"), "不该再有「通讯录」这行标题文字");
    }
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    QVERIFY2(search != nullptr && search->parentWidget() == conversationList->parentWidget(),
             "搜索框应当就在会话列表这一列的头部");
    auto* contactsSearch = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    QVERIFY2(contactsSearch != nullptr
                 && contactsSearch->parentWidget() == contactsList->parentWidget(),
             "通讯录搜索框应当就在联系人列表这一列的头部");
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

// 通讯录页的搜索框必须搜联系人（而不是搜消息），且要和消息搜索一样容错：
// 严格 contains 的话，备注名记岔一个字就什么都搜不出来。
void MainWindowLayoutTest::contactSearchFiltersDirectoryFuzzily() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone 手机"));
    app.addContact(QStringLiteral("mac-agent"), QStringLiteral("Mac 构建机"));
    app.addContact(QStringLiteral("desk-01"), QStringLiteral("台式机"));

    MainWindow window(app);
    auto* contactsSearch = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsSearch != nullptr);
    QVERIFY(contactsList != nullptr);
    QCOMPARE(contactsList->count(), 3);

    auto visibleNames = [contactsList] {
        QStringList names;
        for (int row = 0; row < contactsList->count(); ++row) {
            if (contactsList->item(row)->isHidden()) continue;
            names << contactsList->item(row)->data(Qt::UserRole + 1).toString();
        }
        names.sort();
        return names;
    };

    QCOMPARE(visibleNames().size(), 3);

    // 原样子串。
    contactsSearch->setText(QStringLiteral("Mac"));
    QCOMPARE(visibleNames(), QStringList{QStringLiteral("Mac 构建机")});

    // 子序列：「构机」在「Mac 构建机」里按顺序出现但中间隔了字。
    // 这一条正是严格匹配会漏掉的，也是这个测试真正要守住的行为。
    contactsSearch->setText(QStringLiteral("构机"));
    QCOMPARE(visibleNames(), QStringList{QStringLiteral("Mac 构建机")});

    // 备注名里没有「desk」，只有 userId 有——ID 也要能搜。
    contactsSearch->setText(QStringLiteral("desk"));
    QCOMPARE(visibleNames(), QStringList{QStringLiteral("台式机")});

    // 谁都不像的词应当一条都不剩，而不是「搜不到就全给你」。
    contactsSearch->setText(QStringLiteral("zzzz"));
    QVERIFY(visibleNames().isEmpty());

    // 清空恢复全部。
    contactsSearch->clear();
    QCOMPARE(visibleNames().size(), 3);

    // 通讯录页按 Ctrl+F 应当聚焦到这个框，而不是消息页那个看不见的框。
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(contactsNavButton != nullptr);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    contactsNavButton->click();
    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(contactsSearch->hasFocus());
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
    RemoteIMMessage outgoingMessage;
    outgoingMessage.id = QStringLiteral("outgoing-avatar-anchor");
    outgoingMessage.fromUserId = QStringLiteral("desktop-user");
    outgoingMessage.toUserId = QStringLiteral("phone-user");
    outgoingMessage.direction = RemoteIMMessageDirection::Outgoing;
    outgoingMessage.status = RemoteIMMessageStatus::Sent;
    outgoingMessage.text = QStringLiteral("用于定位本方头像列");
    app.chatState().appendMessageForRestore(outgoingMessage);

    MainWindow window(app);
    window.resize(1680, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    fakeClient->emitIncomingText(QStringLiteral("phone-user"),
                                 QStringLiteral("这是一段比较长的 AICLI 输出内容，用来验证桌面宽屏下消息气泡不会过窄，"
                                                "否则右侧会出现大片没有意义的空白，文本也会被迫换成太多行。"));

    auto* incomingBubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    QVERIFY(incomingBubble != nullptr);
    auto* incomingAvatar = window.findChild<QLabel*>(QStringLiteral("messageAvatarIncoming"));
    QVERIFY(incomingAvatar != nullptr);
    QCOMPARE(incomingAvatar->text(), QStringLiteral("IP"));
    QCOMPARE(incomingAvatar->property("avatarUserId").toString(), QStringLiteral("phone-user"));
    QCOMPARE(incomingAvatar->minimumSize(), QSize(40, 40));
    QTRY_VERIFY2(incomingBubble->maximumWidth() >= 820,
                 qPrintable(QStringLiteral("max=%1 min=%2")
                                .arg(incomingBubble->maximumWidth())
                                .arg(incomingBubble->minimumWidth())));
    QTRY_VERIFY(incomingBubble->minimumWidth() >= 820);

    auto* messageContainer = window.findChild<QWidget*>(QStringLiteral("messageContainer"));
    QVERIFY(messageContainer != nullptr);
    auto* outgoingAvatar = window.findChild<QLabel*>(QStringLiteral("messageAvatarOutgoing"));
    QVERIFY(outgoingAvatar != nullptr);
    auto* outgoingRow = outgoingAvatar->parentWidget();
    QVERIFY(outgoingRow != nullptr);
    auto* outgoingTime = outgoingRow->findChild<QLabel*>(QStringLiteral("messageTimeLabel"));
    auto* outgoingBubble = outgoingRow->findChild<QWidget*>(QStringLiteral("messageBubbleOutgoing"));
    QVERIFY(outgoingTime != nullptr);
    QVERIFY(outgoingBubble != nullptr);
    QTRY_VERIFY2(qAbs((outgoingAvatar->mapTo(outgoingRow, QPoint(0, 0)).y()
                       + outgoingAvatar->height() / 2)
                      - ((outgoingTime->mapTo(outgoingRow, QPoint(0, 0)).y()
                          + outgoingTime->height() / 2
                          + outgoingBubble->mapTo(outgoingRow, QPoint(0, 0)).y())
                         / 2))
                <= 2,
                qPrintable(QStringLiteral("avatarY=%1 avatarH=%2 timeY=%3 timeH=%4 bubbleY=%5")
                               .arg(outgoingAvatar->mapTo(outgoingRow, QPoint(0, 0)).y())
                               .arg(outgoingAvatar->height())
                               .arg(outgoingTime->mapTo(outgoingRow, QPoint(0, 0)).y())
                               .arg(outgoingTime->height())
                               .arg(outgoingBubble->mapTo(outgoingRow, QPoint(0, 0)).y())));
    QTRY_VERIFY(incomingBubble->mapTo(messageContainer, QPoint(0, 0)).x()
                    + incomingBubble->width()
                <= outgoingAvatar->mapTo(messageContainer, QPoint(0, 0)).x());
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

void MainWindowLayoutTest::deleteKeyClearsMessagesButKeepsContactInConversationList() {
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

    // 会话列表只清空聊天记录：好友保留（删除好友是通讯录的功能）。
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 0);
    QCOMPARE(app.chatState().contacts().size(), 2);
    QCOMPARE(app.chatState().contacts().first().userId, QStringLiteral("phone-user"));
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

void MainWindowLayoutTest::everyNavButtonIsStyledConsistently() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    // 导航按钮样式是按 objectName 逐个列举的，新增一项时极易漏进选择器——
    // 漏了就掉回 QPushButton 默认居中布局，与其余项对不齐（远程页上线时就踩过）。
    // 这里断言每个导航按钮都出现在样式表里，且共用同一条左对齐规则。
    const QString styleSheet = window.styleSheet();
    const QStringList navObjectNames{
        QStringLiteral("messagesNavButton"), QStringLiteral("contactsNavButton"),
        QStringLiteral("remoteNavButton"), QStringLiteral("settingsNavButton")};

    for (const QString& name : navObjectNames) {
        auto* button = window.findChild<QPushButton*>(name);
        QVERIFY2(button != nullptr, qPrintable(name));
        QVERIFY2(styleSheet.contains(QStringLiteral("#%1 ").arg(name))
                     || styleSheet.contains(QStringLiteral("#%1,").arg(name)),
                 qPrintable(QStringLiteral("%1 缺少基础样式规则").arg(name)));
        QVERIFY2(styleSheet.contains(QStringLiteral("#%1[selected=\"true\"]").arg(name)),
                 qPrintable(QStringLiteral("%1 缺少选中态样式规则").arg(name)));
    }

    // 所有导航按钮左边缘应当一致（同在导航栏布局里，宽度相同）。
    const int width = window.findChild<QPushButton*>(navObjectNames.first())->width();
    for (const QString& name : navObjectNames) {
        QCOMPARE(window.findChild<QPushButton*>(name)->width(), width);
    }
}

void MainWindowLayoutTest::navLogoUsesAppIconBrandGradient() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    // 品牌色块与应用图标同款渐变（#5B9BFF → #1E40AF）。渐变与字母已改为
    // 离屏生成位图（QPainterPath 灰度抗锯齿），不再经 QSS 背景绘制——
    // 校验 navLogo 持有生成的位图且尺寸随 DPR 放大。
    auto* logo = window.findChild<QLabel*>(QStringLiteral("navLogo"));
    QVERIFY(logo != nullptr);
    const QPixmap pm = logo->pixmap(Qt::ReturnByValue);
    QVERIFY(!pm.isNull());
    QVERIFY(pm.width() >= 34);
    // 渐变角像素应为品牌蓝系（非透明、非白）。
    const QImage img = pm.toImage();
    const QColor center = img.pixelColor(img.width() / 2, img.height() / 8);
    QVERIFY(center.alpha() > 200);
    QVERIFY(center.blue() > center.red());
}

void MainWindowLayoutTest::connectionStatusLivesOnNavAvatarWithoutText() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.resize(1280, 820);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* statusDot = window.findChild<QLabel*>(QStringLiteral("connectionStatusDot"));
    auto* logoContainer = window.findChild<QWidget*>(QStringLiteral("navLogoContainer"));
    auto* chatHeader = window.findChild<QWidget*>(QStringLiteral("chatHeader"));
    QVERIFY(statusDot != nullptr);
    QVERIFY(logoContainer != nullptr);
    QVERIFY(chatHeader != nullptr);
    QCOMPARE(statusDot->parentWidget(), logoContainer);
    QVERIFY2(!chatHeader->isAncestorOf(statusDot), "连接状态仍然占在聊天页右上角");
    auto* logo = logoContainer->findChild<QLabel*>(QStringLiteral("navLogo"));
    QVERIFY(logo != nullptr);
    QVERIFY2(statusDot->x() > logo->x() + logo->width() / 2,
             "连接状态点没有落在头像右侧");
    QVERIFY2(statusDot->y() < logo->y() + logo->height() / 2,
             "连接状态点没有落在头像上方");
    QCOMPARE(statusDot->text(), QString());
    QCOMPARE(statusDot->property("connected").toBool(), false);

    app.connectToService(1, QStringLiteral("test-user-sig"));
    QCOMPARE(statusDot->property("connected").toBool(), true);
    QCOMPARE(statusDot->text(), QString());
    QVERIFY(statusDot->toolTip().contains(QStringLiteral("已连接")));
}

void MainWindowLayoutTest::fileBubbleOffersContextMenu() {
    QTemporaryDir dir;
    const QString sourcePath = dir.filePath(QStringLiteral("report.md"));
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("# 报告\n正文");
    }

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveFile(QStringLiteral("phone-user"), sourcePath,
                                QStringLiteral("report.md"), QStringLiteral("text/markdown"), 10);

    MainWindow window(app);
    auto* fileButton = window.findChild<QPushButton*>(QStringLiteral("messageFileButton"));
    QVERIFY(fileButton != nullptr);
    // 自定义右键菜单（预览 / 保存到本地）挂在文件气泡按钮上。
    QCOMPARE(fileButton->contextMenuPolicy(), Qt::CustomContextMenu);
}

void MainWindowLayoutTest::imageBubbleOffersContextMenu() {
    QTemporaryDir dir;
    const QString imagePath = dir.filePath(QStringLiteral("shot.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath));

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveImage(QStringLiteral("phone-user"), imagePath, 8, 8, 100);

    MainWindow window(app);
    auto* imageLabel = window.findChild<QLabel*>(QStringLiteral("messageImageLabel"));
    QVERIFY(imageLabel != nullptr);
    // 自定义右键菜单（复制 / 预览 / 保存到本地）挂在图片缩略图上。
    QCOMPARE(imageLabel->contextMenuPolicy(), Qt::CustomContextMenu);
}

void MainWindowLayoutTest::maximizedImageBubbleOpensOnlyOnePreview() {
    QTemporaryDir dir;
    const QString imagePath = dir.filePath(QStringLiteral("shot.png"));
    const QSize originalSize(320, 180);
    QImage image(originalSize, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath));

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveImage(QStringLiteral("phone-user"), imagePath,
                                 originalSize.width(), originalSize.height(), 100);

    MainWindow window(app);
    window.showMaximized();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* imageLabel = window.findChild<QLabel*>(QStringLiteral("messageImageLabel"));
    QVERIFY(imageLabel != nullptr);

    // 连续到达的点击事件也只能复用当前预览，不能叠出多个预览窗口。
    QTest::mouseClick(imageLabel, Qt::LeftButton);
    QTest::mouseClick(imageLabel, Qt::LeftButton);
    QTRY_COMPARE(window.findChildren<ImagePreviewDialog*>().size(), 1);
    auto* preview = window.findChild<ImagePreviewDialog*>(QStringLiteral("imagePreviewDialog"));
    QVERIFY(preview != nullptr);
    QVERIFY(preview->isVisible());
    QVERIFY(!(preview->windowState() & Qt::WindowFullScreen));
    QVERIFY(!preview->windowFlags().testFlag(Qt::FramelessWindowHint));
    QCOMPARE(preview->size(), originalSize);

    const QSize resizedPreview(420, 260);
    preview->resize(resizedPreview);
    QTRY_COMPARE(preview->size(), resizedPreview);

    preview->accept();
    QTRY_VERIFY(window.findChildren<ImagePreviewDialog*>().isEmpty());
}

void MainWindowLayoutTest::copyAttachmentToPathCopiesOverwritesAndReportsErrors() {
    QTemporaryDir dir;
    const QString sourcePath = dir.filePath(QStringLiteral("weekly.md"));
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("# 周报内容");
    }
    RemoteIMFileAttachment attachment{sourcePath, QStringLiteral("weekly.md"), QStringLiteral("text/markdown"), 0};

    // 正常保存：内容一致。
    const QString targetPath = dir.filePath(QStringLiteral("saved/weekly.md"));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("saved")));
    QString error;
    QVERIFY(MainWindow::copyAttachmentToPath(attachment, targetPath, &error));
    QFile saved(targetPath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArray("# 周报内容"));
    saved.close();

    // 目标已存在：覆盖（「另存为」对话框已确认过覆盖语义）。
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        source.write("# 更新后的周报");
    }
    QVERIFY(MainWindow::copyAttachmentToPath(attachment, targetPath, &error));
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArray("# 更新后的周报"));
    saved.close();

    // 源缓存缺失（未下载完成/被清理）：失败并给出原因。
    RemoteIMFileAttachment missing{dir.filePath(QStringLiteral("gone.md")), QStringLiteral("gone.md"), QString(), 0};
    QVERIFY(!MainWindow::copyAttachmentToPath(missing, dir.filePath(QStringLiteral("out.md")), &error));
    QVERIFY(!error.isEmpty());

    // 空目标路径：失败。
    QVERIFY(!MainWindow::copyAttachmentToPath(attachment, QStringLiteral("  "), &error));
    QVERIFY(!error.isEmpty());
}

void MainWindowLayoutTest::ctrlShortcutsZoomWholeUi() {
    UiZoom::setFactor(1.0);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // Ctrl+= 放大一档并弹出百分比浮层。
    QTest::keyClick(&window, Qt::Key_Equal, Qt::ControlModifier);
    QCOMPARE(qRound(UiZoom::factor() * 100), 110);
    auto* toast = window.findChild<QLabel*>(QStringLiteral("zoomToast"));
    QVERIFY(toast != nullptr);
    QVERIFY(toast->isVisible());
    QCOMPARE(toast->text(), QStringLiteral("110%"));
    // 代码级最小宽高随倍率重放（520 × 1.1 = 572）。
    auto* chatContentPane = window.findChild<QWidget*>(QStringLiteral("chatContentPane"));
    QVERIFY(chatContentPane != nullptr);
    QCOMPARE(chatContentPane->minimumWidth(), 572);
    // 导航栏缩放后仍须是定宽窄条。这里重放的曾经是「带文字时代」的 160~260，
    // 会把构造时的定宽顶掉——缩放一次导航栏就胖回去，还在旁边留出一条空白列。
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    QVERIFY(navRail != nullptr);
    QCOMPARE(navRail->minimumWidth(), navRail->maximumWidth());
    QCOMPARE(navRail->maximumWidth(), 70);  // 64 × 1.1

    // Ctrl+- 缩回，Ctrl+0 复位；最小宽高须一并还原，否则布局缩不回去。
    QTest::keyClick(&window, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(qRound(UiZoom::factor() * 100), 100);
    QCOMPARE(chatContentPane->minimumWidth(), 520);
    QTest::keyClick(&window, Qt::Key_Equal, Qt::ControlModifier);
    QTest::keyClick(&window, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(qRound(UiZoom::factor() * 100), 100);
    QCOMPARE(chatContentPane->minimumWidth(), 520);
    QCOMPARE(window.minimumWidth(), 980);

    UiZoom::setFactor(1.0);
}


// 搜索入口在顶栏，搜的是所有会话——所以这条必须跨两个会话验，
// 只在当前会话里能搜到的话，把它放在顶栏就是误导。
void MainWindowLayoutTest::globalSearchFindsMatchesAcrossConversationsAndJumps() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("peer-a"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("peer-b"), QStringLiteral("Bob"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);

    app.selectPeer(QStringLiteral("peer-a"));
    editor->setPlainText(QStringLiteral("构建失败了，看下 CMake"));
    sendButton->click();
    app.selectPeer(QStringLiteral("peer-b"));
    editor->setPlainText(QStringLiteral("cmake 已经修好"));
    sendButton->click();
    editor->setPlainText(QStringLiteral("与关键词无关的一条"));
    sendButton->click();

    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    auto* results = window.findChild<QListWidget*>(QStringLiteral("globalSearchResults"));
    QVERIFY(search != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(results->isHidden());

    // 搜索是防抖的：输入后要等停顿才真正执行，所以这里必须等，不能同步断言。
    // 防抖本身是为了不让每次按键都去扫所有会话，卡顿就是那样来的。
    search->setText(QStringLiteral("cmake"));
    QTRY_VERIFY(!results->isHidden());
    QTRY_COMPARE(results->count(), 2);

    // 左边的会话列表不能被清空：这两个会话都含命中，虽然它们的名字和
    // 最后一条预览都不含关键词。否则右边列着结果、左边一片空白，自相矛盾。
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(conversationList != nullptr);
    int visibleConversations = 0;
    for (int i = 0; i < conversationList->count(); ++i) {
        if (!conversationList->item(i)->isHidden()) ++visibleConversations;
    }
    QCOMPARE(visibleConversations, 2);

    // 结果里要能看出各自属于哪个会话，否则跨会话的结果混在一起没法用。
    QStringList shown;
    for (int i = 0; i < results->count(); ++i) shown << results->item(i)->text();
    QVERIFY(shown.filter(QStringLiteral("Alice")).size() == 1);
    QVERIFY(shown.filter(QStringLiteral("Bob")).size() == 1);

    // 点 Alice 那条：应当切回 peer-a 并高亮那一行。当前选中的是 peer-b，
    // 所以这条同时验证了「跨会话跳转」。
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("peer-b"));
    QListWidgetItem* target = nullptr;
    for (int i = 0; i < results->count(); ++i) {
        if (results->item(i)->text().contains(QStringLiteral("Alice"))) target = results->item(i);
    }
    QVERIFY(target != nullptr);
    emit results->itemClicked(target);

    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("peer-a"));
    QVERIFY(results->isHidden());
    // 跳转要等下一轮事件循环（切会话会整屏重建气泡），所以这里用 QTRY。
    QTRY_COMPARE(highlightedRowCount(window), 1);
}

// 搜不到时必须说清范围：没点过「加载更早」的历史不在内存里，也就搜不到。
// 只显示「无结果」会让人以为这句话从没说过。
// 搜索把左列过滤成「有命中的会话」之后，点其中一个必须直接落到命中那条消息。
// 此前只有右侧结果面板会跳，左列点进去只是打开会话——筛出来了还得自己找。
void MainWindowLayoutTest::clickingFilteredConversationJumpsToItsSearchHit() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("peer-a"), QStringLiteral("Peer A"));
    app.addContact(QStringLiteral("peer-b"), QStringLiteral("Peer B"));
    // peer-b 里夹一条不相关的，确保跳的是命中那条而不是最后一条。
    app.chatState().receiveText(QStringLiteral("peer-b"), QStringLiteral("水管漏了要修"));
    app.chatState().receiveText(QStringLiteral("peer-b"), QStringLiteral("构建失败在链接那步"));
    app.chatState().receiveText(QStringLiteral("peer-b"), QStringLiteral("今天天气不错"));
    app.selectPeer(QStringLiteral("peer-a"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(search != nullptr);
    QVERIFY(conversationList != nullptr);
    QCOMPARE(highlightedRowCount(window), 0);

    search->setText(QStringLiteral("构建失败"));
    // 搜索有 150ms 防抖，过滤要等它跑完。
    QTRY_VERIFY(conversationList->count() > 0);

    int targetRow = -1;
    for (int row = 0; row < conversationList->count(); ++row) {
        QListWidgetItem* item = conversationList->item(row);
        if (item->isHidden()) continue;
        if (item->data(Qt::UserRole).toString() == QStringLiteral("peer-b")) targetRow = row;
    }
    QVERIFY2(targetRow >= 0, "有命中的会话必须留在过滤后的左列里");

    conversationList->setCurrentRow(targetRow);
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("peer-b"));
    // 定位延后到事件循环下一轮，所以用 QTRY。
    QTRY_COMPARE(highlightedRowCount(window), 1);
}

void MainWindowLayoutTest::globalSearchReportsNoResultWithLoadedScopeHint() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    editor->setPlainText(QStringLiteral("只有这一条"));
    sendButton->click();

    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    auto* results = window.findChild<QListWidget*>(QStringLiteral("globalSearchResults"));
    QVERIFY(search != nullptr);
    QVERIFY(results != nullptr);

    search->setText(QStringLiteral("不存在的关键词"));
    QTRY_COMPARE(results->count(), 1);
    const QString hint = results->item(0)->text();
    QVERIFY(hint.contains(QStringLiteral("无结果")));
    QVERIFY(hint.contains(QStringLiteral("已加载")));
    // 提示项不能被点开——它不对应任何消息。
    QVERIFY(!results->item(0)->flags().testFlag(Qt::ItemIsSelectable));

    // 清空输入立即收起面板，不走防抖——没人愿意清空后还要等 150ms 才恢复。
    search->clear();
    QVERIFY(results->isHidden());
}

namespace {

// 通讯录列表的一行：要么是分组表头，要么是联系人。
struct DirectoryRow {
    bool isHeader = false;
    QString text;    // 表头是分组名，联系人是显示名
    QString group;   // 所属/所指分组，空串 = 未分组
    int count = 0;   // 仅表头有意义
    bool hidden = false;
};

QList<DirectoryRow> readDirectory(QListWidget* list) {
    QList<DirectoryRow> rows;
    for (int index = 0; index < list->count(); ++index) {
        QListWidgetItem* item = list->item(index);
        DirectoryRow row;
        row.isHeader = item->data(Qt::UserRole + 8).toBool();
        row.text = item->data(Qt::UserRole + 1).toString();
        row.group = item->data(Qt::UserRole + 9).toString();
        row.count = item->data(Qt::UserRole + 11).toInt();
        row.hidden = item->isHidden();
        rows.append(row);
    }
    return rows;
}

// 只保留没被过滤掉的行，写断言时不必跟着隐藏行数数。
QStringList visibleLabels(QListWidget* list) {
    QStringList labels;
    for (const DirectoryRow& row : readDirectory(list)) {
        if (row.hidden) continue;
        labels.append(row.isHeader ? QStringLiteral("[%1 %2]").arg(row.text).arg(row.count) : row.text);
    }
    return labels;
}

QListWidget* openContactsPage(MainWindow& window) {
    auto* navButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    if (navButton) navButton->click();
    return window.findChild<QListWidget*>(QStringLiteral("contactsList"));
}

// 找到某个分组表头当前所在的行。列表每次刷新都会重建，不能缓存这个下标。
int headerRowOf(QListWidget* list, const QString& groupName) {
    for (int index = 0; index < list->count(); ++index) {
        QListWidgetItem* item = list->item(index);
        if (item->data(Qt::UserRole + 8).toBool()
            && item->data(Qt::UserRole + 9).toString() == groupName) {
            return index;
        }
    }
    return -1;
}

}  // namespace

void MainWindowLayoutTest::contactDirectoryStaysFlatUntilAGroupExists() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    // 一个分组都没建过的人不该被迫看见分组结构：连「未分组」表头也不该出现。
    QCOMPARE(visibleLabels(list), QStringList({"Alice", "Bob"}));
    for (const DirectoryRow& row : readDirectory(list)) {
        QVERIFY(!row.isHeader);
    }
}

void MainWindowLayoutTest::contactDirectoryShowsGroupsWithUngroupedLast() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    QVERIFY(app.createContactGroup(QStringLiteral("家人")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    // 分组按创建顺序；「家人」还没人也要显示，否则新建分组看起来什么都没发生。
    // 没有分组的 Bob 不套任何标题，直接跟在分组后面、和分组同一层。
    QCOMPARE(visibleLabels(list),
             QStringList({"[同事 1]", "Alice", "[家人 0]", "Bob"}));

    // 移动之后 Bob 落到「家人」下面，没有分组的人一个不剩，列表里也不该
    // 冒出一个空的「未分组」标题。
    app.setContactGroup(QStringLiteral("bob"), QStringLiteral("家人"));
    QCOMPARE(visibleLabels(list),
             QStringList({"[同事 1]", "Alice", "[家人 1]", "Bob"}));
}

void MainWindowLayoutTest::clickingGroupHeaderCollapsesItsMembers() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    const int headerRow = headerRowOf(list, QStringLiteral("同事"));
    QVERIFY(headerRow >= 0);
    emit list->itemClicked(list->item(headerRow));

    // 收起后成员不见了，但表头和人数还在——那是唯一还能看出组里有人的线索。
    // 没有分组的 Bob 不属于任何一节，折叠影响不到他。
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Bob"}));

    emit list->itemClicked(list->item(headerRowOf(list, QStringLiteral("同事"))));
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Alice", "Bob"}));
}

void MainWindowLayoutTest::contactSearchLooksInsideCollapsedGroups() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);
    emit list->itemClicked(list->item(headerRowOf(list, QStringLiteral("同事"))));
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Bob"}));

    auto* search = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    QVERIFY(search != nullptr);
    search->setText(QStringLiteral("Alice"));

    // 命中的人藏在收起的分组里不显示，看上去就是「搜不到」。搜索必须无视折叠。
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Alice"}));

    // 清空搜索后回到收起状态，折叠不会因为搜过一次就丢了。
    search->clear();
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Bob"}));
}

void MainWindowLayoutTest::contactSearchHidesGroupsWithoutAnyHit() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    QVERIFY(app.createContactGroup(QStringLiteral("家人")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    auto* search = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    QVERIFY(search != nullptr);
    search->setText(QStringLiteral("Alice"));

    // 一个命中都没有的分组，表头也要藏掉，否则结果里全是空标题。
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Alice"}));

    // Bob 没有分组：搜到他时，两个分组的表头都该消失，
    // 不能因为他排在分组后面就把上一个分组的标题留下来。
    search->setText(QStringLiteral("Bob"));
    QCOMPARE(visibleLabels(list), QStringList({"Bob"}));
}

void MainWindowLayoutTest::groupHeaderCountStaysTheGroupSizeWhileSearching() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("amy"), QStringLiteral("Amy"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));
    app.setContactGroup(QStringLiteral("amy"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);
    QCOMPARE(visibleLabels(list), QStringList({"[同事 2]", "Alice", "Amy", "Bob"}));

    auto* search = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    QVERIFY(search != nullptr);
    search->setText(QStringLiteral("Alice"));

    // 只有 Alice 命中，但标题上的数字仍然是 2。
    // 那个数字说的是「这个分组有几个人」，不是「搜到了几个」——
    // 跟着搜索变的话，一边搜一边看，分组的人数就成了会跳的数字。
    QCOMPARE(visibleLabels(list), QStringList({"[同事 2]", "Alice"}));
}

QTEST_MAIN(MainWindowLayoutTest)
#include "MainWindowLayoutTest.moc"
