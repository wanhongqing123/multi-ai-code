#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#include <QAbstractItemView>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QtMath>
#include <functional>
#include <utility>

#include "im/RemoteIMCredentialDefaults.h"
#include "markdown/MarkdownRenderer.h"
#include "ui/ImagePreviewDialog.h"

namespace {

constexpr int UserIdRole = Qt::UserRole;
constexpr int DisplayNameRole = Qt::UserRole + 1;
constexpr int PreviewRole = Qt::UserRole + 2;
constexpr int TimeRole = Qt::UserRole + 3;

class MarkdownMessageView final : public QTextBrowser {
public:
    explicit MarkdownMessageView(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setObjectName(QStringLiteral("messageMarkdownView"));
        setFrameShape(QFrame::NoFrame);
        setReadOnly(true);
        setOpenExternalLinks(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDocumentMargin(0);
        viewport()->setAutoFillBackground(false);
        setStyleSheet(QStringLiteral(R"(
            QTextBrowser {
                color: #172033;
                background: transparent;
                border: 0;
                font-size: 13px;
            }
            QTextBrowser a {
                color: #0b67b7;
            }
        )"));
    }

    void setMessageMarkdown(const QString& markdown) {
        setHtml(MarkdownRenderer::renderToHtml(markdown));
        updateContentHeight();
    }

    QSize sizeHint() const override {
        return QSize(360, qMax(24, qCeil(document()->size().height()) + 2));
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        updateContentHeight();
    }

private:
    void updateContentHeight() {
        const int width = qMax(120, viewport()->width());
        if (!qFuzzyCompare(document()->textWidth(), static_cast<qreal>(width))) {
            document()->setTextWidth(width);
        }
        setFixedHeight(qMax(24, qCeil(document()->size().height()) + 2));
        updateGeometry();
    }
};

class ConversationListDelegate final : public QStyledItemDelegate {
public:
    explicit ConversationListDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, 76);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect rowRect = option.rect.adjusted(0, 2, -6, -2);
        if (option.state & QStyle::State_Selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#dff3ff")));
            painter->drawRoundedRect(rowRect, 0, 0);
        }

        const QRect avatarRect(rowRect.left() + 12, rowRect.top() + 10, 40, 40);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#168eea")));
        painter->drawRoundedRect(avatarRect, 8, 8);
        QFont avatarFont = option.font;
        avatarFont.setBold(true);
        painter->setFont(avatarFont);
        painter->setPen(Qt::white);
        painter->drawText(avatarRect, Qt::AlignCenter, QStringLiteral("IM"));

        const int textLeft = avatarRect.right() + 14;
        const int timeWidth = 48;
        const QRect timeRect(rowRect.right() - timeWidth - 8, rowRect.top() + 12, timeWidth, 18);
        const QRect nameRect(textLeft, rowRect.top() + 12, timeRect.left() - textLeft - 10, 20);
        const QRect previewRect(textLeft, rowRect.top() + 41, rowRect.right() - textLeft - 12, 18);

        const QString name = index.data(DisplayNameRole).toString();
        const QString preview = index.data(PreviewRole).toString();
        const QString time = index.data(TimeRole).toString();

        QFont nameFont = option.font;
        nameFont.setPointSize(15);
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(QColor(QStringLiteral("#101828")));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameRect.width()));

        QFont timeFont = option.font;
        timeFont.setPointSize(11);
        painter->setFont(timeFont);
        painter->setPen(QColor(QStringLiteral("#98a2b3")));
        painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, time);

        QFont previewFont = option.font;
        previewFont.setPointSize(12);
        painter->setFont(previewFont);
        painter->setPen(QColor(QStringLiteral("#667085")));
        painter->drawText(previewRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(previewFont).elidedText(preview, Qt::ElideRight, previewRect.width()));

        painter->restore();
    }
};

class ContactListDelegate final : public QStyledItemDelegate {
public:
    explicit ContactListDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, 54);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect rowRect = option.rect.adjusted(0, 2, -6, -2);
        if (option.state & QStyle::State_Selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#dff3ff")));
            painter->drawRoundedRect(rowRect, 0, 0);
        }

        const QRect avatarRect(rowRect.left() + 12, rowRect.top() + 7, 36, 36);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#168eea")));
        painter->drawRoundedRect(avatarRect, 8, 8);
        QFont avatarFont = option.font;
        avatarFont.setBold(true);
        painter->setFont(avatarFont);
        painter->setPen(Qt::white);
        painter->drawText(avatarRect, Qt::AlignCenter, QStringLiteral("IM"));

        const int textLeft = avatarRect.right() + 14;
        const QRect nameRect(textLeft, rowRect.top(), rowRect.right() - textLeft - 12, rowRect.height());

        const QString name = index.data(DisplayNameRole).toString();

        QFont nameFont = option.font;
        nameFont.setPointSize(14);
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(QColor(QStringLiteral("#101828")));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameRect.width()));

        painter->restore();
    }
};

class ClickableImageLabel final : public QLabel {
public:
    explicit ClickableImageLabel(QString imagePath, std::function<void(const QString&)> onClick, QWidget* parent = nullptr)
        : QLabel(parent), imagePath_(std::move(imagePath)), onClick_(std::move(onClick)) {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClick_) {
            onClick_(imagePath_);
            return;
        }
        QLabel::mousePressEvent(event);
    }

private:
    QString imagePath_;
    std::function<void(const QString&)> onClick_;
};

QPushButton* makeNavButton(const QString& title, const QString& objectName, QWidget* parent) {
    auto* button = new QPushButton(title, parent);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QPushButton* makeHeaderButton(const QString& title, QWidget* parent) {
    auto* button = new QPushButton(title, parent);
    button->setObjectName(QStringLiteral("headerButton"));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QString deliveryStatusIndicator(RemoteIMMessageStatus status) {
    switch (status) {
        case RemoteIMMessageStatus::Pending:
            return QString();
        case RemoteIMMessageStatus::Sent:
            return QStringLiteral("✓");
        case RemoteIMMessageStatus::Failed:
            return QStringLiteral("!");
        case RemoteIMMessageStatus::Received:
            return QString();
    }
    return QString();
}

QString latestMessageText(const QList<RemoteIMMessage>& messages) {
    if (messages.isEmpty()) return QStringLiteral("暂无消息");
    QString text = messages.last().text;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text;
}

QString relativeMessageTimeText(qint64 createdAtMillis) {
    const QDateTime messageTime = QDateTime::fromMSecsSinceEpoch(createdAtMillis);
    const QDate messageDate = messageTime.date();
    const QDate today = QDate::currentDate();
    if (messageDate == today) return messageTime.toString(QStringLiteral("HH:mm"));
    if (messageDate == today.addDays(-1)) return QStringLiteral("昨天 ") + messageTime.toString(QStringLiteral("HH:mm"));
    return messageTime.toString(QStringLiteral("M 月 d 日 HH:mm"));
}

QString latestMessageTime(const QList<RemoteIMMessage>& messages) {
    if (messages.isEmpty()) return QString();
    return relativeMessageTimeText(messages.last().createdAtMillis);
}

QString messageTimeText(const RemoteIMMessage& message) {
    return relativeMessageTimeText(message.createdAtMillis);
}

}  // namespace

MainWindow::MainWindow(RemoteIMApplication& app, QWidget* parent) : QMainWindow(parent), app_(app) {
    buildUi();
    applyStyle();
    bindSignals();
    refresh();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == messageEditor_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool isReturn = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        if (isReturn && (keyEvent->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            messageEditor_->insertPlainText(QStringLiteral("\n"));
            return true;
        }
        if (isReturn && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            sendCurrentText();
            return true;
        }
    }
    if (messageScroll_ && watched == messageScroll_->viewport() && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
}

void MainWindow::buildUi() {
    setWindowTitle(QString());
    resize(1280, 820);
    setMinimumSize(980, 640);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("root"));
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    setCentralWidget(root);

    auto* rootNavigationSplitter = new QSplitter(Qt::Horizontal, root);
    rootNavigationSplitter->setObjectName(QStringLiteral("rootNavigationSplitter"));
    rootNavigationSplitter->setChildrenCollapsible(false);
    rootNavigationSplitter->setHandleWidth(6);

    contentStack_ = new QStackedWidget(rootNavigationSplitter);
    contentStack_->setObjectName(QStringLiteral("contentStack"));

    messagesPage_ = new QWidget(contentStack_);
    messagesPage_->setObjectName(QStringLiteral("messagesPage"));
    auto* messagesPageLayout = new QHBoxLayout(messagesPage_);
    messagesPageLayout->setContentsMargins(0, 0, 0, 0);
    messagesPageLayout->setSpacing(0);

    auto* contentSplitter = new QSplitter(Qt::Horizontal, messagesPage_);
    contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
    contentSplitter->setChildrenCollapsible(false);
    contentSplitter->setHandleWidth(6);

    navRail_ = new QWidget(rootNavigationSplitter);
    navRail_->setObjectName(QStringLiteral("navRail"));
    navRail_->setMinimumWidth(160);
    navRail_->setMaximumWidth(260);
    auto* navLayout = new QVBoxLayout(navRail_);
    navLayout->setContentsMargins(16, 18, 16, 14);
    navLayout->setSpacing(12);

    auto* logo = new QLabel(QStringLiteral("M"), navRail_);
    logo->setObjectName(QStringLiteral("navLogo"));
    logo->setAlignment(Qt::AlignCenter);
    addContactButton_ = new QPushButton(QStringLiteral("+"), navRail_);
    addContactButton_->setObjectName(QStringLiteral("addConversationButton"));
    addContactButton_->setToolTip(QStringLiteral("添加联系人"));
    addContactButton_->setCursor(Qt::PointingHandCursor);

    auto* navTopRow = new QHBoxLayout();
    navTopRow->setContentsMargins(0, 0, 0, 0);
    navTopRow->setSpacing(12);
    navTopRow->addWidget(logo, 0, Qt::AlignLeft);
    navTopRow->addStretch(1);
    navTopRow->addWidget(addContactButton_, 0, Qt::AlignRight);
    navLayout->addLayout(navTopRow);
    navLayout->addSpacing(8);
    messageNavButton_ = makeNavButton(QStringLiteral("消息"), QStringLiteral("messagesNavButton"), navRail_);
    contactsNavButton_ = makeNavButton(QStringLiteral("通讯录"), QStringLiteral("contactsNavButton"), navRail_);
    settingsNavButton_ = makeNavButton(QStringLiteral("设置"), QStringLiteral("settingsNavButton"), navRail_);
    messageNavButton_->setProperty("selected", true);
    navLayout->addWidget(messageNavButton_);
    navLayout->addWidget(contactsNavButton_);
    navLayout->addWidget(settingsNavButton_);
    navLayout->addStretch(1);

    auto* conversationPane = new QWidget(messagesPage_);
    conversationPane->setObjectName(QStringLiteral("conversationPane"));
    conversationPane->setMinimumWidth(220);
    auto* conversationLayout = new QVBoxLayout(conversationPane);
    conversationLayout->setContentsMargins(20, 18, 16, 16);
    conversationLayout->setSpacing(14);

    auto* conversationHeader = new QHBoxLayout();
    auto* messagesTitle = new QLabel(QStringLiteral("消息"), conversationPane);
    messagesTitle->setObjectName(QStringLiteral("messagesSectionTitle"));
    conversationHeader->addWidget(messagesTitle);
    conversationHeader->addStretch();

    conversationList_ = new QListWidget(conversationPane);
    conversationList_->setObjectName(QStringLiteral("conversationList"));
    conversationList_->setFrameShape(QFrame::NoFrame);
    conversationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversationList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    conversationList_->setUniformItemSizes(true);
    conversationList_->setItemDelegate(new ConversationListDelegate(conversationList_));

    conversationLayout->addLayout(conversationHeader);
    conversationLayout->addWidget(conversationList_, 1);

    auto* chatContentPane = new QWidget(messagesPage_);
    chatContentPane->setObjectName(QStringLiteral("chatContentPane"));
    chatContentPane->setMinimumWidth(520);
    auto* chatLayout = new QVBoxLayout(chatContentPane);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    auto* header = new QWidget(chatContentPane);
    header->setObjectName(QStringLiteral("chatHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(28, 18, 28, 18);
    headerLayout->setSpacing(12);
    titleLabel_ = new QLabel(header);
    titleLabel_->setObjectName(QStringLiteral("chatTitle"));
    statusLabel_ = new QLabel(QStringLiteral("未连接"), header);
    statusLabel_->setObjectName(QStringLiteral("statusBadge"));
    headerLayout->addWidget(titleLabel_, 1);
    headerLayout->addWidget(makeHeaderButton(QStringLiteral("搜索"), header));
    headerLayout->addWidget(makeHeaderButton(QStringLiteral("更多"), header));
    headerLayout->addWidget(statusLabel_);

    messageScroll_ = new QScrollArea(chatContentPane);
    messageScroll_->setObjectName(QStringLiteral("messageScroll"));
    messageScroll_->setWidgetResizable(true);
    messageScroll_->setFrameShape(QFrame::NoFrame);
    messageScroll_->viewport()->installEventFilter(this);
    messageContainer_ = new QWidget(messageScroll_);
    messageContainer_->setObjectName(QStringLiteral("messageContainer"));
    messageLayout_ = new QVBoxLayout(messageContainer_);
    messageLayout_->setObjectName(QStringLiteral("messageLayout"));
    messageLayout_->setContentsMargins(28, 22, 28, 22);
    messageLayout_->setSpacing(14);
    messageScroll_->setWidget(messageContainer_);

    auto* composer = new QWidget(chatContentPane);
    composer->setObjectName(QStringLiteral("composerPanel"));
    composer->setMinimumHeight(116);
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(24, 12, 24, 14);
    composerLayout->setSpacing(8);

    messageEditor_ = new QTextEdit(composer);
    messageEditor_->setObjectName(QStringLiteral("messageEditor"));
    messageEditor_->setPlaceholderText(QStringLiteral("输入消息"));
    messageEditor_->setAcceptRichText(false);
    messageEditor_->setMinimumHeight(64);
    messageEditor_->installEventFilter(this);

    auto* toolBar = new QHBoxLayout();
    toolBar->setObjectName(QStringLiteral("composerToolbar"));
    toolBar->setContentsMargins(0, 0, 0, 0);
    toolBar->setSpacing(10);
    voiceButton_ = new QPushButton(composer);
    voiceButton_->setObjectName(QStringLiteral("toolIconButton"));
    voiceButton_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    voiceButton_->setToolTip(QStringLiteral("语音消息"));
    voiceButton_->setCursor(Qt::PointingHandCursor);

    imageButton_ = new QPushButton(QStringLiteral("+"), composer);
    imageButton_->setObjectName(QStringLiteral("toolIconButton"));
    imageButton_->setToolTip(QStringLiteral("发送图片"));
    imageButton_->setCursor(Qt::PointingHandCursor);

    sendButton_ = new QPushButton(QStringLiteral("发送"), composer);
    sendButton_->setObjectName(QStringLiteral("sendButton"));
    sendButton_->setCursor(Qt::PointingHandCursor);

    toolBar->addWidget(voiceButton_);
    toolBar->addWidget(imageButton_);
    toolBar->addStretch(1);
    toolBar->addWidget(sendButton_);

    composerLayout->addWidget(messageEditor_, 1);
    composerLayout->addLayout(toolBar);

    auto* messageComposerSplitter = new QSplitter(Qt::Vertical, chatContentPane);
    messageComposerSplitter->setObjectName(QStringLiteral("messageComposerSplitter"));
    messageComposerSplitter->setChildrenCollapsible(false);
    messageComposerSplitter->setHandleWidth(6);
    messageComposerSplitter->addWidget(messageScroll_);
    messageComposerSplitter->addWidget(composer);
    messageComposerSplitter->setStretchFactor(0, 1);
    messageComposerSplitter->setStretchFactor(1, 0);
    messageComposerSplitter->setSizes(QList<int>() << 620 << 166);

    chatLayout->addWidget(header);
    chatLayout->addWidget(messageComposerSplitter, 1);

    contentSplitter->addWidget(conversationPane);
    contentSplitter->addWidget(chatContentPane);
    contentSplitter->setStretchFactor(0, 0);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setSizes(QList<int>() << 320 << 960);
    messagesPageLayout->addWidget(contentSplitter, 1);

    contactsPage_ = new QWidget(contentStack_);
    contactsPage_->setObjectName(QStringLiteral("contactsPage"));
    auto* contactsPageLayout = new QHBoxLayout(contactsPage_);
    contactsPageLayout->setContentsMargins(0, 0, 0, 0);
    contactsPageLayout->setSpacing(0);

    auto* contactsDirectoryPane = new QWidget(contactsPage_);
    contactsDirectoryPane->setObjectName(QStringLiteral("contactsDirectoryPane"));
    contactsDirectoryPane->setMinimumWidth(300);
    contactsDirectoryPane->setMaximumWidth(420);
    auto* contactsDirectoryLayout = new QVBoxLayout(contactsDirectoryPane);
    contactsDirectoryLayout->setContentsMargins(24, 24, 20, 18);
    contactsDirectoryLayout->setSpacing(16);

    auto* contactsHeader = new QHBoxLayout();
    auto* contactsTitle = new QLabel(QStringLiteral("通讯录"), contactsDirectoryPane);
    contactsTitle->setObjectName(QStringLiteral("contactsSectionTitle"));
    contactsHeader->addWidget(contactsTitle);
    contactsHeader->addStretch(1);

    contactsList_ = new QListWidget(contactsDirectoryPane);
    contactsList_->setObjectName(QStringLiteral("contactsList"));
    contactsList_->setFrameShape(QFrame::NoFrame);
    contactsList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contactsList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    contactsList_->setUniformItemSizes(true);
    contactsList_->setItemDelegate(new ContactListDelegate(contactsList_));
    contactsDirectoryLayout->addLayout(contactsHeader);
    contactsDirectoryLayout->addWidget(contactsList_, 1);

    auto* contactHintPane = new QWidget(contactsPage_);
    contactHintPane->setObjectName(QStringLiteral("contactHintPane"));
    auto* contactHintLayout = new QVBoxLayout(contactHintPane);
    contactHintLayout->setContentsMargins(36, 36, 36, 36);
    contactHintLayout->setSpacing(10);
    auto* contactHintTitle = new QLabel(QStringLiteral("选择联系人开始会话"), contactHintPane);
    contactHintTitle->setObjectName(QStringLiteral("contactHintTitle"));
    auto* contactHintSubtitle = new QLabel(QStringLiteral("点击左侧联系人后会切回消息页，并打开对应聊天窗口。"), contactHintPane);
    contactHintSubtitle->setObjectName(QStringLiteral("contactHintSubtitle"));
    contactHintLayout->addStretch(1);
    contactHintLayout->addWidget(contactHintTitle, 0, Qt::AlignHCenter);
    contactHintLayout->addWidget(contactHintSubtitle, 0, Qt::AlignHCenter);
    contactHintLayout->addStretch(1);

    contactsPageLayout->addWidget(contactsDirectoryPane);
    contactsPageLayout->addWidget(contactHintPane, 1);

    settingsPage_ = new QWidget(contentStack_);
    settingsPage_->setObjectName(QStringLiteral("settingsPage"));
    auto* settingsLayout = new QVBoxLayout(settingsPage_);
    settingsLayout->setContentsMargins(36, 30, 36, 30);
    settingsLayout->setSpacing(18);

    auto* settingsTitle = new QLabel(QStringLiteral("设置"), settingsPage_);
    settingsTitle->setObjectName(QStringLiteral("pageTitle"));
    auto* settingsSubtitle = new QLabel(QStringLiteral("桌面端 IM 使用和移动端一致的内置连接配置。"), settingsPage_);
    settingsSubtitle->setObjectName(QStringLiteral("settingsSubtitle"));

    auto* settingsPanel = new QWidget(settingsPage_);
    settingsPanel->setObjectName(QStringLiteral("settingsPanel"));
    auto* settingsPanelLayout = new QVBoxLayout(settingsPanel);
    settingsPanelLayout->setContentsMargins(0, 0, 0, 0);
    settingsPanelLayout->setSpacing(0);

    settingsAccountValue_ = new QLabel(settingsPanel);
    settingsAccountValue_->setObjectName(QStringLiteral("settingsAccountValue"));
    settingsConnectionValue_ = new QLabel(settingsPanel);
    settingsConnectionValue_->setObjectName(QStringLiteral("settingsConnectionValue"));
    settingsSdkAppIdValue_ = new QLabel(settingsPanel);
    settingsSdkAppIdValue_->setObjectName(QStringLiteral("settingsSdkAppIdValue"));
    auto* signatureValue = new QLabel(QStringLiteral("内置生成"), settingsPanel);
    signatureValue->setObjectName(QStringLiteral("settingsSignatureValue"));

    settingsPanelLayout->addWidget(createSettingsRow(QStringLiteral("当前账号"), settingsAccountValue_, QStringLiteral("用于登录桌面端 IM 的账号 ID。")));
    settingsPanelLayout->addWidget(createSettingsRow(QStringLiteral("连接状态"), settingsConnectionValue_, QStringLiteral("显示当前 SDK 登录状态。")));
    settingsPanelLayout->addWidget(createSettingsRow(QStringLiteral("SDK AppID"), settingsSdkAppIdValue_, QStringLiteral("和 iOS 使用同一套内置配置。")));
    settingsPanelLayout->addWidget(createSettingsRow(QStringLiteral("登录签名"), signatureValue, QStringLiteral("启动时自动生成，不需要用户手动填写。")));

    settingsLayout->addWidget(settingsTitle);
    settingsLayout->addWidget(settingsSubtitle);
    settingsLayout->addWidget(settingsPanel);
    settingsLayout->addStretch(1);

    contentStack_->addWidget(messagesPage_);
    contentStack_->addWidget(contactsPage_);
    contentStack_->addWidget(settingsPage_);

    rootNavigationSplitter->addWidget(navRail_);
    rootNavigationSplitter->addWidget(contentStack_);
    rootNavigationSplitter->setStretchFactor(0, 0);
    rootNavigationSplitter->setStretchFactor(1, 1);
    rootNavigationSplitter->setSizes(QList<int>() << 180 << 1100);
    rootLayout->addWidget(rootNavigationSplitter, 1);
}

void MainWindow::applyStyle() {
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, #root {
            background: #f6f9fc;
            color: #172033;
            font-size: 14px;
        }
        #navRail {
            background: #ecf3ff;
        }
        #navLogo {
            min-width: 42px;
            max-width: 42px;
            min-height: 42px;
            max-height: 42px;
            border-radius: 10px;
            background: #168eea;
            color: #ffffff;
            font-size: 18px;
            font-weight: 800;
        }
        #messagesNavButton, #contactsNavButton, #settingsNavButton {
            min-height: 42px;
            border: 0;
            border-radius: 8px;
            background: transparent;
            color: #62728a;
            font-size: 13px;
            font-weight: 700;
            text-align: left;
            padding-left: 22px;
            padding-right: 12px;
        }
        #messagesNavButton[selected="true"], #contactsNavButton[selected="true"], #settingsNavButton[selected="true"] {
            background: #dff1ff;
            color: #0b67b7;
        }
        #conversationPane {
            background: #ffffff;
            border-right: 1px solid #dae4f0;
        }
        #messagesSectionTitle, #contactsSectionTitle, #pageTitle {
            color: #101828;
            font-size: 22px;
            font-weight: 800;
        }
        #addConversationButton, #headerButton, #toolIconButton {
            min-width: 42px;
            max-width: 42px;
            min-height: 42px;
            max-height: 42px;
            border-radius: 10px;
            border: 1px solid #dae4f0;
            background: #ffffff;
            color: #172033;
            font-size: 20px;
            font-weight: 800;
        }
        #headerButton {
            min-width: 56px;
            max-width: 56px;
            font-size: 13px;
            font-weight: 700;
            color: #667085;
        }
        #addConversationButton:hover, #headerButton:hover, #toolIconButton:hover {
            background: #edf7ff;
            border-color: #8ed0ff;
        }
        #conversationList {
            background: transparent;
            outline: 0;
        }
        #conversationList::item {
            min-height: 72px;
            padding: 0;
            color: #344054;
        }
        #conversationList::item:selected {
            background: #e1f5ff;
            border: 0;
        }
        #contactsPage, #settingsPage {
            background: #ffffff;
        }
        #contactsDirectoryPane {
            background: #ffffff;
            border-right: 1px solid #dae4f0;
        }
        #contactHintPane {
            background: #ffffff;
        }
        #contactsList {
            background: transparent;
            outline: 0;
        }
        #contactsList::item {
            min-height: 52px;
            padding: 0;
            color: #344054;
        }
        #contactsList::item:selected {
            background: #e1f5ff;
            border: 0;
        }
        #contactHintTitle {
            color: #101828;
            font-size: 18px;
            font-weight: 800;
        }
        #contactHintSubtitle, #settingsSubtitle {
            color: #667085;
            font-size: 13px;
        }
        #settingsPanel {
            background: #ffffff;
            border: 1px solid #dae4f0;
            border-radius: 8px;
        }
        #settingsRow {
            background: #ffffff;
            border-bottom: 1px solid #edf2f7;
        }
        #settingsRowTitle {
            color: #172033;
            font-size: 14px;
            font-weight: 800;
        }
        #settingsRowHelper {
            color: #667085;
            font-size: 12px;
        }
        #settingsAccountValue, #settingsConnectionValue, #settingsSdkAppIdValue, #settingsSignatureValue {
            color: #172033;
            font-size: 14px;
            font-weight: 700;
        }
        #chatContentPane {
            background: #ffffff;
        }
        #chatHeader {
            background: #ffffff;
            border-bottom: 1px solid #dae4f0;
        }
        #chatTitle {
            color: #101828;
            font-size: 21px;
            font-weight: 800;
        }
        #statusBadge {
            background: #e7f8ee;
            color: #087443;
            border-radius: 8px;
            padding: 7px 14px;
            font-size: 14px;
            font-weight: 800;
        }
        #messageScroll, #messageContainer {
            background: #ffffff;
        }
        #composerPanel {
            background: #ffffff;
            border-top: 1px solid #dae4f0;
        }
        #messageEditor {
            border: 1px solid #dae4f0;
            border-radius: 14px;
            background: #ffffff;
            color: #172033;
            padding: 10px 13px;
            font-size: 14px;
        }
        #messageEditor:focus {
            border-color: #58b7ff;
        }
        #sendButton {
            min-width: 78px;
            min-height: 34px;
            border-radius: 7px;
            border: 0;
            background: #168eea;
            color: #ffffff;
            font-size: 14px;
            font-weight: 800;
        }
        #sendButton:disabled {
            background: #c4def0;
            color: #f5fbff;
        }
        QSplitter::handle {
            background: #edf2f8;
        }
        QSplitter::handle:horizontal {
            width: 6px;
        }
        QSplitter::handle:vertical {
            height: 6px;
        }
        QSplitter::handle:hover {
            background: #c7d8ea;
        }
    )"));
}

void MainWindow::bindSignals() {
    connect(&app_, &RemoteIMApplication::stateChanged, this, [this] { refresh(); });
    connect(&app_, &RemoteIMApplication::connectionChanged, this, [this](bool connected) {
        statusLabel_->setText(connected ? QStringLiteral("● 已连接") : QStringLiteral("● 未连接"));
        refreshSettings();
    });
    connect(&app_, &RemoteIMApplication::errorMessage, this, [this](const QString& message) {
        if (QCoreApplication::arguments().contains(QStringLiteral("--smoke"))) return;
        QMessageBox::warning(this, QStringLiteral("IM"), message);
    });
    connect(addContactButton_, &QPushButton::clicked, this, [this] { openAddContactDialog(); });
    connect(messageNavButton_, &QPushButton::clicked, this, [this] { showMessagesPage(); });
    connect(contactsNavButton_, &QPushButton::clicked, this, [this] { showContactsPage(); });
    connect(settingsNavButton_, &QPushButton::clicked, this, [this] { showSettingsPage(); });
    connect(contentStack_, &QStackedWidget::currentChanged, this, [this] { syncNavigationSelection(); });
    connect(imageButton_, &QPushButton::clicked, this, [this] { openImagePicker(); });
    connect(voiceButton_, &QPushButton::clicked, this, [this] { app_.sendVoicePlaceholder(); });
    connect(sendButton_, &QPushButton::clicked, this, [this] { sendCurrentText(); });
    connect(messageEditor_, &QTextEdit::textChanged, this, [this] { updateComposerState(); });
    connect(conversationList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
        if (!current) return;
        const QString userId = current->data(Qt::UserRole).toString();
        if (!userId.isEmpty() && userId != app_.chatState().selectedPeerId()) app_.selectPeer(userId);
    });
    auto openContactConversation = [this](QListWidgetItem* item) {
        if (!item) return;
        const QString userId = item->data(Qt::UserRole).toString();
        if (userId.isEmpty()) return;
        app_.selectPeer(userId);
        showMessagesPage();
    };
    connect(contactsList_, &QListWidget::itemClicked, this, openContactConversation);
    connect(contactsList_, &QListWidget::itemActivated, this, openContactConversation);
}

void MainWindow::refresh() {
    refreshContacts();
    refreshContactDirectory();
    refreshSettings();
    refreshMessages();
}

void MainWindow::refreshContacts() {
    const QString selectedPeer = app_.chatState().selectedPeerId();
    conversationList_->blockSignals(true);
    conversationList_->clear();
    int selectedRow = -1;
    const QList<RemoteIMContact> contacts = app_.chatState().contacts();
    for (int index = 0; index < contacts.size(); ++index) {
        const RemoteIMContact& contact = contacts[index];
        auto* item = new QListWidgetItem();
        item->setSizeHint(QSize(0, 76));
        const QList<RemoteIMMessage> messages = app_.chatState().messagesWith(contact.userId);
        item->setData(UserIdRole, contact.userId);
        item->setData(DisplayNameRole, contact.displayName.isEmpty() ? contact.userId : contact.displayName);
        item->setData(PreviewRole, latestMessageText(messages));
        item->setData(TimeRole, latestMessageTime(messages));
        conversationList_->addItem(item);
        if (contact.userId == selectedPeer) selectedRow = index;
    }
    if (selectedRow >= 0) conversationList_->setCurrentRow(selectedRow);
    conversationList_->blockSignals(false);
}

void MainWindow::refreshContactDirectory() {
    contactsList_->blockSignals(true);
    contactsList_->clear();
    const QList<RemoteIMContact> contacts = app_.chatState().contacts();
    for (const RemoteIMContact& contact : contacts) {
        auto* item = new QListWidgetItem();
        item->setSizeHint(QSize(0, 54));
        item->setData(UserIdRole, contact.userId);
        item->setData(DisplayNameRole, contact.displayName.isEmpty() ? contact.userId : contact.displayName);
        contactsList_->addItem(item);
    }
    contactsList_->blockSignals(false);
}

void MainWindow::refreshSettings() {
    settingsAccountValue_->setText(app_.chatState().ownerUserId());
    settingsConnectionValue_->setText(app_.isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    settingsSdkAppIdValue_->setText(QString::number(RemoteIMCredentialDefaults::sdkAppId));
}

void MainWindow::showMessagesPage() {
    contentStack_->setCurrentWidget(messagesPage_);
    syncNavigationSelection();
    messageEditor_->setFocus();
}

void MainWindow::showContactsPage() {
    refreshContactDirectory();
    contentStack_->setCurrentWidget(contactsPage_);
    syncNavigationSelection();
}

void MainWindow::showSettingsPage() {
    refreshSettings();
    contentStack_->setCurrentWidget(settingsPage_);
    syncNavigationSelection();
}

void MainWindow::syncNavigationSelection() {
    if (contentStack_->currentWidget() == contactsPage_) {
        updateNavigationSelection(contactsNavButton_);
        return;
    }
    if (contentStack_->currentWidget() == settingsPage_) {
        updateNavigationSelection(settingsNavButton_);
        return;
    }
    updateNavigationSelection(messageNavButton_);
}

void MainWindow::updateNavigationSelection(QPushButton* selectedButton) {
    const QList<QPushButton*> buttons = {messageNavButton_, contactsNavButton_, settingsNavButton_};
    for (QPushButton* button : buttons) {
        if (!button) continue;
        button->setProperty("selected", button == selectedButton);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
}

void MainWindow::refreshMessages() {
    const QString selectedPeer = app_.chatState().selectedPeerId();
    titleLabel_->setText(selectedPeer.isEmpty() ? QStringLiteral("请选择会话") : contactName(selectedPeer));
    statusLabel_->setText(app_.isConnected() ? QStringLiteral("● 已连接") : QStringLiteral("● 未连接"));
    updateComposerState();

    while (QLayoutItem* item = messageLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) delete widget;
        delete item;
    }

    const QList<RemoteIMMessage> messages = app_.chatState().messagesWith(selectedPeer);
    if (messages.isEmpty()) {
        auto* emptyView = new QWidget(messageContainer_);
        emptyView->setObjectName(QStringLiteral("emptyMessagesView"));
        auto* emptyLayout = new QVBoxLayout(emptyView);
        emptyLayout->setContentsMargins(0, 52, 0, 0);
        emptyLayout->setSpacing(10);

        auto* iconLabel = new QLabel(QStringLiteral("◇"), emptyView);
        iconLabel->setObjectName(QStringLiteral("emptyMessageIcon"));
        iconLabel->setAlignment(Qt::AlignCenter);
        auto* title = new QLabel(QStringLiteral("暂无消息"), emptyView);
        title->setObjectName(QStringLiteral("emptyMessageTitle"));
        title->setAlignment(Qt::AlignCenter);
        auto* subtitle = new QLabel(QStringLiteral("发送一条消息开始远程任务。"), emptyView);
        subtitle->setObjectName(QStringLiteral("emptyMessageSubtitle"));
        subtitle->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(iconLabel);
        emptyLayout->addWidget(title);
        emptyLayout->addWidget(subtitle);
        emptyLayout->addStretch(1);
        emptyView->setStyleSheet(QStringLiteral(R"(
            #emptyMessageIcon {
                color: #98a2b3;
                font-size: 28px;
                background: transparent;
            }
            #emptyMessageTitle {
                color: #101828;
                font-size: 16px;
                font-weight: 800;
                background: transparent;
            }
            #emptyMessageSubtitle {
                color: #667085;
                font-size: 13px;
                background: transparent;
            }
        )"));
        messageLayout_->addWidget(emptyView);
    } else {
        for (const RemoteIMMessage& message : messages) {
            messageLayout_->addWidget(createMessageBubble(message));
        }
        messageLayout_->addStretch(1);
    }

    QTimer::singleShot(0, this, [this] {
        messageScroll_->verticalScrollBar()->setValue(messageScroll_->verticalScrollBar()->maximum());
    });
}

void MainWindow::openAddContactDialog() {
    bool ok = false;
    const QString userId = QInputDialog::getText(this,
                                                QStringLiteral("添加联系人"),
                                                QStringLiteral("账号 ID"),
                                                QLineEdit::Normal,
                                                QString(),
                                                &ok)
                               .trimmed();
    if (!ok || userId.isEmpty()) return;
    app_.addContact(userId, userId);
}

void MainWindow::openImagePicker() {
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("选择图片"),
                                                      QString(),
                                                      QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
    if (!path.isEmpty()) app_.sendImage(path);
}

void MainWindow::openImagePreview(const QString& imagePath) {
    ImagePreviewDialog dialog(imagePath, this);
    dialog.showFullScreen();
    dialog.exec();
}

QWidget* MainWindow::createMessageBubble(const RemoteIMMessage& message) {
    const bool outgoing = message.direction == RemoteIMMessageDirection::Outgoing;
    auto* row = new QWidget(messageContainer_);
    row->setObjectName(outgoing ? QStringLiteral("messageRowOutgoing") : QStringLiteral("messageRowIncoming"));
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);

    auto* bubble = new QWidget(row);
    bubble->setObjectName(outgoing ? QStringLiteral("messageBubbleOutgoing") : QStringLiteral("messageBubbleIncoming"));
    const bool expandedTextBubble = !message.hasImage && (message.text.size() >= 50 || message.text.contains(QLatin1Char('\n')));
    bubble->setProperty("expandedTextBubble", expandedTextBubble);
    applyMessageBubbleWidth(bubble, expandedTextBubble);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    bubble->setStyleSheet(outgoing
                              ? QStringLiteral("#messageBubbleOutgoing{background:#ffffff;border:1px solid #c3dffd;border-radius:8px;}")
                              : QStringLiteral("#messageBubbleIncoming{background:#fffbed;border:1px solid #fdd058;border-radius:8px;}"));

    auto* bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(13, 10, 13, 10);
    bubbleLayout->setSpacing(7);

    auto* metaRow = new QHBoxLayout();
    metaRow->setContentsMargins(0, 0, 0, 0);
    metaRow->setSpacing(8);
    auto* authorLabel = new QLabel(message.fromUserId, bubble);
    authorLabel->setObjectName(QStringLiteral("messageAuthorLabel"));
    auto* timeLabel = new QLabel(messageTimeText(message), bubble);
    timeLabel->setObjectName(QStringLiteral("messageTimeLabel"));
    metaRow->addWidget(authorLabel);
    if (!outgoing) {
        auto* relationLabel = new QLabel(QStringLiteral("好友"), bubble);
        relationLabel->setObjectName(QStringLiteral("messageRelationBadge"));
        metaRow->addWidget(relationLabel);
    }
    metaRow->addWidget(timeLabel);
    metaRow->addStretch(1);
    bubbleLayout->addLayout(metaRow);

    auto* contentRow = new QHBoxLayout();
    contentRow->setContentsMargins(0, 0, 0, 0);
    contentRow->setSpacing(10);

    if (message.hasImage) {
        QPixmap pixmap(message.image.localPath);
        if (!pixmap.isNull()) {
            auto* imageLabel = new ClickableImageLabel(message.image.localPath, [this](const QString& path) {
                openImagePreview(path);
            }, bubble);
            const QPixmap thumbnail = pixmap.scaled(QSize(280, 200), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            imageLabel->setPixmap(thumbnail);
            imageLabel->setAlignment(Qt::AlignCenter);
            imageLabel->setMinimumSize(thumbnail.size());
            contentRow->addWidget(imageLabel);
        } else {
            auto* missingLabel = new QLabel(QStringLiteral("图片无法加载"), bubble);
            missingLabel->setWordWrap(true);
            contentRow->addWidget(missingLabel);
        }
    } else {
        auto* markdownView = new MarkdownMessageView(bubble);
        markdownView->setMessageMarkdown(message.text);
        contentRow->addWidget(markdownView, 1);
    }

    const QString status = outgoing ? deliveryStatusIndicator(message.status) : QString();
    if (!status.isEmpty()) {
        auto* statusLabel = new QLabel(status, bubble);
        statusLabel->setObjectName(QStringLiteral("messageStatusLabel"));
        statusLabel->setAlignment(Qt::AlignCenter);
        statusLabel->setFixedSize(16, 16);
        statusLabel->setStyleSheet(QStringLiteral(R"(
            QLabel#messageStatusLabel {
                border: 1px solid #12a150;
                border-radius: 8px;
                background: transparent;
                color: #12a150;
                font-size: 11px;
                font-weight: 800;
                padding: 0;
            }
        )"));
        contentRow->addWidget(statusLabel, 0, Qt::AlignVCenter);
    }
    bubbleLayout->addLayout(contentRow);
    bubble->setStyleSheet(bubble->styleSheet() + QStringLiteral(R"(
        #messageAuthorLabel {
            color: #101828;
            font-size: 13px;
            font-weight: 800;
            background: transparent;
        }
        #messageTimeLabel {
            color: #667085;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        #messageRelationBadge {
            background: #e7f8ee;
            border: 0;
            border-radius: 7px;
            color: #087443;
            padding: 2px 7px;
            font-size: 12px;
            font-weight: 700;
        }
    )"));

    if (outgoing) rowLayout->addSpacing(42);
    if (outgoing) rowLayout->addStretch(1);
    rowLayout->addWidget(bubble);
    if (!outgoing) {
        rowLayout->addStretch(1);
        rowLayout->addSpacing(42);
    }
    return row;
}

int MainWindow::messageBubbleMaximumWidth() const {
    int viewportWidth = messageScroll_ && messageScroll_->viewport() ? messageScroll_->viewport()->width() : 0;
    if (viewportWidth < 700) {
        viewportWidth = qMax(viewportWidth, width() - 560);
    }
    return qBound(620, viewportWidth - 96, 1320);
}

void MainWindow::applyMessageBubbleWidth(QWidget* bubble, bool expanded) const {
    if (!bubble) return;
    const int maximumWidth = messageBubbleMaximumWidth();
    bubble->setMaximumWidth(maximumWidth);
    bubble->setMinimumWidth(expanded ? maximumWidth : 0);
}

void MainWindow::updateMessageBubbleWidths() {
    QList<QWidget*> bubbles = messageContainer_->findChildren<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    bubbles.append(messageContainer_->findChildren<QWidget*>(QStringLiteral("messageBubbleOutgoing")));
    for (QWidget* bubble : bubbles) {
        applyMessageBubbleWidth(bubble, bubble->property("expandedTextBubble").toBool());
    }
}

QWidget* MainWindow::createSettingsRow(const QString& title, QLabel* valueLabel, const QString& helperText) {
    auto* row = new QWidget(valueLabel ? valueLabel->parentWidget() : settingsPage_);
    row->setObjectName(QStringLiteral("settingsRow"));
    row->setMinimumHeight(72);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(18, 12, 18, 12);
    layout->setSpacing(20);

    auto* textColumn = new QVBoxLayout();
    textColumn->setContentsMargins(0, 0, 0, 0);
    textColumn->setSpacing(4);
    auto* titleLabel = new QLabel(title, row);
    titleLabel->setObjectName(QStringLiteral("settingsRowTitle"));
    auto* helperLabel = new QLabel(helperText, row);
    helperLabel->setObjectName(QStringLiteral("settingsRowHelper"));
    helperLabel->setWordWrap(true);
    textColumn->addWidget(titleLabel);
    textColumn->addWidget(helperLabel);

    if (valueLabel) {
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valueLabel->setTextFormat(Qt::PlainText);
        valueLabel->setMinimumWidth(180);
    }

    layout->addLayout(textColumn, 1);
    if (valueLabel) layout->addWidget(valueLabel);
    return row;
}

void MainWindow::sendCurrentText() {
    const QString text = messageEditor_->toPlainText().trimmed();
    if (text.isEmpty() || app_.chatState().selectedPeerId().isEmpty()) return;
    app_.sendText(text);
    messageEditor_->clear();
    updateComposerState();
}

void MainWindow::updateComposerState() {
    const bool hasPeer = !app_.chatState().selectedPeerId().isEmpty();
    const bool hasText = messageEditor_ && !messageEditor_->toPlainText().trimmed().isEmpty();
    imageButton_->setEnabled(hasPeer);
    messageEditor_->setEnabled(hasPeer);
    voiceButton_->setEnabled(hasPeer);
    sendButton_->setEnabled(hasPeer && hasText);
}

QString MainWindow::contactName(const QString& userId) const {
    for (const RemoteIMContact& contact : app_.chatState().contacts()) {
        if (contact.userId == userId) return contact.displayName.isEmpty() ? contact.userId : contact.displayName;
    }
    return userId;
}
