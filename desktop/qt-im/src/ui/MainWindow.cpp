#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QAction>
#include <QDateTime>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
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
#include <QVariant>
#include <QtMath>
#include <functional>
#include <utility>

#include "im/RemoteIMCredentialDefaults.h"
#include "markdown/MarkdownRenderer.h"
#include "ui/AddContactDialog.h"
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
        return QSize(0, 72);
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

        const QString name = index.data(DisplayNameRole).toString();
        const QString preview = index.data(PreviewRole).toString();
        const QString time = index.data(TimeRole).toString();

        const int textLeft = avatarRect.right() + 14;
        // Size the time column to the actual text so short "HH:mm" stamps free up
        // room for the name and long dates never clip.
        QFont timeFont = option.font;
        timeFont.setPixelSize(12);
        const int timeWidth = time.isEmpty() ? 0 : QFontMetrics(timeFont).horizontalAdvance(time) + 2;
        const QRect timeRect(rowRect.right() - timeWidth - 10, rowRect.top() + 12, timeWidth, 18);
        const int nameRight = time.isEmpty() ? rowRect.right() - 12 : timeRect.left() - 10;
        const QRect nameRect(textLeft, rowRect.top() + 12, qMax(0, nameRight - textLeft), 20);
        const QRect previewRect(textLeft, rowRect.top() + 41, rowRect.right() - textLeft - 12, 18);

        QFont nameFont = option.font;
        nameFont.setPixelSize(14);
        nameFont.setWeight(QFont::Medium);
        painter->setFont(nameFont);
        painter->setPen(QColor(QStringLiteral("#1f2329")));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameRect.width()));

        painter->setFont(timeFont);
        painter->setPen(QColor(QStringLiteral("#98a2b3")));
        painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, time);

        QFont previewFont = option.font;
        previewFont.setPixelSize(13);
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
        nameFont.setPixelSize(14);
        nameFont.setWeight(QFont::Medium);
        painter->setFont(nameFont);
        painter->setPen(QColor(QStringLiteral("#1f2329")));
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

enum class LineIconKind {
    Messages = 1,
    Contacts,
    Settings,
    Search,
    Add,
    More,
};

int lineIconKindValue(LineIconKind kind) {
    return static_cast<int>(kind);
}

LineIconKind lineIconKindFromValue(int value) {
    switch (static_cast<LineIconKind>(value)) {
        case LineIconKind::Messages:
        case LineIconKind::Contacts:
        case LineIconKind::Settings:
        case LineIconKind::Search:
        case LineIconKind::Add:
        case LineIconKind::More:
            return static_cast<LineIconKind>(value);
    }
    return LineIconKind::Messages;
}

QIcon makeLineIcon(LineIconKind kind, const QColor& color) {
    constexpr int kRender = 48;
    QPixmap pixmap(kRender, kRender);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
        case LineIconKind::Messages:
            painter.drawRoundedRect(QRectF(10, 12, 28, 22), 6, 6);
            painter.drawLine(QPointF(17, 20), QPointF(31, 20));
            painter.drawLine(QPointF(17, 27), QPointF(27, 27));
            painter.drawLine(QPointF(18, 34), QPointF(14, 40));
            break;
        case LineIconKind::Contacts:
            painter.drawEllipse(QRectF(18, 10, 12, 12));
            painter.drawArc(QRectF(13, 21, 22, 18), 28 * 16, 124 * 16);
            painter.drawEllipse(QRectF(30, 15, 8, 8));
            painter.drawArc(QRectF(27, 24, 16, 13), 20 * 16, 105 * 16);
            break;
        case LineIconKind::Settings:
            painter.drawEllipse(QRectF(17, 17, 14, 14));
            painter.drawLine(QPointF(24, 7), QPointF(24, 12));
            painter.drawLine(QPointF(24, 36), QPointF(24, 41));
            painter.drawLine(QPointF(7, 24), QPointF(12, 24));
            painter.drawLine(QPointF(36, 24), QPointF(41, 24));
            painter.drawLine(QPointF(12, 12), QPointF(16, 16));
            painter.drawLine(QPointF(32, 32), QPointF(36, 36));
            painter.drawLine(QPointF(36, 12), QPointF(32, 16));
            painter.drawLine(QPointF(16, 32), QPointF(12, 36));
            break;
        case LineIconKind::Search:
            painter.drawEllipse(QRectF(11, 11, 20, 20));
            painter.drawLine(QPointF(29, 29), QPointF(38, 38));
            break;
        case LineIconKind::Add:
            painter.drawLine(QPointF(24, 13), QPointF(24, 35));
            painter.drawLine(QPointF(13, 24), QPointF(35, 24));
            break;
        case LineIconKind::More:
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(QRectF(13, 21, 6, 6));
            painter.drawEllipse(QRectF(21, 21, 6, 6));
            painter.drawEllipse(QRectF(29, 21, 6, 6));
            break;
    }
    painter.end();
    return QIcon(pixmap);
}

QPushButton* makeNavButton(const QString& title, const QString& objectName, QWidget* parent) {
    auto* button = new QPushButton(title, parent);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

void applyNavButtonIcon(QPushButton* button, bool selected) {
    const QVariant rawKind = button->property("navIconKind");
    if (!rawKind.isValid()) return;
    const QColor color = selected ? QColor(QStringLiteral("#0b67b7")) : QColor(QStringLiteral("#62728a"));
    button->setIcon(makeLineIcon(lineIconKindFromValue(rawKind.toInt()), color));
}

// Feishu-style borderless icon button for the chat header.
QPushButton* makeHeaderIconButton(LineIconKind kind, const QString& tooltip, QWidget* parent) {
    auto* button = new QPushButton(parent);
    button->setObjectName(QStringLiteral("headerIconButton"));
    button->setIcon(makeLineIcon(kind, QColor(QStringLiteral("#4c5866"))));
    button->setIconSize(QSize(17, 17));
    button->setToolTip(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

struct SlashCommandDefinition {
    QString command;
    QString label;
    QString objectName;
};

QList<SlashCommandDefinition> slashCommandDefinitions() {
    return {
        {QStringLiteral("/status"), QStringLiteral("查看状态"), QStringLiteral("slashCommandButton_status")},
        {QStringLiteral("/plan"), QStringLiteral("切换 Plan"), QStringLiteral("slashCommandButton_plan")},
        {QStringLiteral("/build"), QStringLiteral("切换 Build"), QStringLiteral("slashCommandButton_build")},
        {QStringLiteral("/models"), QStringLiteral("模型列表"), QStringLiteral("slashCommandButton_models")},
        {QStringLiteral("/model "), QStringLiteral("切换模型"), QStringLiteral("slashCommandButton_model")},
        {QStringLiteral("/help"), QStringLiteral("命令帮助"), QStringLiteral("slashCommandButton_help")},
    };
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

// Compact timestamp for the conversation list (WeChat/Feishu style): today shows
// the clock, yesterday/older collapse to a date so the narrow time column never
// clips (e.g. "昨天 14:30" would otherwise render as "天 14:30").
QString conversationListTimeText(qint64 createdAtMillis) {
    if (createdAtMillis <= 0) return QString();
    const QDateTime messageTime = QDateTime::fromMSecsSinceEpoch(createdAtMillis);
    const QDate messageDate = messageTime.date();
    const QDate today = QDate::currentDate();
    if (messageDate == today) return messageTime.toString(QStringLiteral("HH:mm"));
    if (messageDate == today.addDays(-1)) return QStringLiteral("昨天");
    if (messageDate.year() == today.year()) return messageTime.toString(QStringLiteral("M月d日"));
    return messageTime.toString(QStringLiteral("yyyy/M/d"));
}

QString latestMessageTime(const QList<RemoteIMMessage>& messages) {
    if (messages.isEmpty()) return QString();
    return conversationListTimeText(messages.last().createdAtMillis);
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
    if ((watched == conversationList_ || watched == contactsList_) && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace) {
            deleteSelectedContactFromList(qobject_cast<QListWidget*>(watched));
            return true;
        }
    }
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
    // 单个空格而不是空串：空标题时 Qt 会回退显示 applicationDisplayName
    // （"Multi-AI Code IM"），飞书风格的标题栏不显示文字。
    setWindowTitle(QStringLiteral(" "));
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
    addContactButton_ = new QPushButton(navRail_);
    addContactButton_->setObjectName(QStringLiteral("addConversationButton"));
    addContactButton_->setIcon(makeLineIcon(LineIconKind::Add, QColor(QStringLiteral("#4c5866"))));
    addContactButton_->setIconSize(QSize(18, 18));
    addContactButton_->setToolTip(QStringLiteral("添加联系人"));
    addContactButton_->setCursor(Qt::PointingHandCursor);

    auto* navTopRow = new QHBoxLayout();
    navTopRow->setContentsMargins(0, 0, 0, 0);
    navTopRow->setSpacing(12);
    navTopRow->addWidget(logo, 0, Qt::AlignLeft);
    navTopRow->addStretch(1);
    navTopRow->addWidget(addContactButton_, 0, Qt::AlignRight);
    navLayout->addLayout(navTopRow);

    navSearchInput_ = new QLineEdit(navRail_);
    navSearchInput_->setObjectName(QStringLiteral("navSearchBox"));
    navSearchInput_->setPlaceholderText(QStringLiteral("搜索"));
    navSearchInput_->setClearButtonEnabled(true);
    navSearchInput_->addAction(makeLineIcon(LineIconKind::Search, QColor(QStringLiteral("#98a2b3"))),
                               QLineEdit::LeadingPosition);
    navLayout->addWidget(navSearchInput_);
    navLayout->addSpacing(8);

    messageNavButton_ = makeNavButton(QStringLiteral("消息"), QStringLiteral("messagesNavButton"), navRail_);
    contactsNavButton_ = makeNavButton(QStringLiteral("通讯录"), QStringLiteral("contactsNavButton"), navRail_);
    settingsNavButton_ = makeNavButton(QStringLiteral("设置"), QStringLiteral("settingsNavButton"), navRail_);
    messageNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Messages));
    contactsNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Contacts));
    settingsNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Settings));
    messageNavButton_->setProperty("selected", true);
    for (QPushButton* navButton : {messageNavButton_, contactsNavButton_, settingsNavButton_}) {
        navButton->setIconSize(QSize(18, 18));
    }
    applyNavButtonIcon(messageNavButton_, true);
    applyNavButtonIcon(contactsNavButton_, false);
    applyNavButtonIcon(settingsNavButton_, false);
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
    headerLayout->addWidget(makeHeaderIconButton(LineIconKind::Search, QStringLiteral("搜索"), header));
    headerLayout->addWidget(makeHeaderIconButton(LineIconKind::More, QStringLiteral("更多"), header));
    headerLayout->addWidget(statusLabel_);

    messageScroll_ = new QScrollArea(chatContentPane);
    messageScroll_->setObjectName(QStringLiteral("messageScroll"));
    messageScroll_->setWidgetResizable(true);
    messageScroll_->setFrameShape(QFrame::NoFrame);
    messageScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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

    slashCommandBar_ = new QWidget(composer);
    slashCommandBar_->setObjectName(QStringLiteral("slashCommandBar"));
    slashCommandBar_->setVisible(false);
    slashCommandLayout_ = new QHBoxLayout(slashCommandBar_);
    slashCommandLayout_->setContentsMargins(0, 0, 0, 0);
    slashCommandLayout_->setSpacing(8);

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

    composerLayout->addWidget(slashCommandBar_);
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
            min-width: 34px;
            max-width: 34px;
            min-height: 34px;
            max-height: 34px;
            border-radius: 17px;
            background: #168eea;
            color: #ffffff;
            font-size: 15px;
            font-weight: 700;
        }
        #navSearchBox {
            min-height: 34px;
            border: 1px solid #dbe6f3;
            border-radius: 9px;
            background: #ffffff;
            color: #172033;
            padding: 0 8px;
            font-size: 13px;
        }
        #navSearchBox:focus {
            border-color: #8ed0ff;
        }
        #messagesNavButton, #contactsNavButton, #settingsNavButton {
            min-height: 40px;
            border: 0;
            border-radius: 8px;
            background: transparent;
            color: #62728a;
            font-size: 14px;
            font-weight: 500;
            text-align: left;
            padding-left: 12px;
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
            font-size: 16px;
            font-weight: 600;
        }
        #toolIconButton {
            min-width: 42px;
            max-width: 42px;
            min-height: 42px;
            max-height: 42px;
            border-radius: 10px;
            border: 1px solid #dae4f0;
            background: #ffffff;
            color: #172033;
            font-size: 18px;
            font-weight: 600;
        }
        #toolIconButton:hover {
            background: #edf7ff;
            border-color: #8ed0ff;
        }
        #headerIconButton {
            min-width: 34px;
            max-width: 34px;
            min-height: 34px;
            max-height: 34px;
            border: 0;
            border-radius: 8px;
            background: transparent;
        }
        #addConversationButton {
            min-width: 34px;
            max-width: 34px;
            min-height: 34px;
            max-height: 34px;
            border: 0;
            border-radius: 17px;
            background: transparent;
        }
        #headerIconButton:hover, #addConversationButton:hover {
            background: #e9eef5;
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
            font-size: 16px;
            font-weight: 600;
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
            font-weight: 600;
        }
        #settingsRowHelper {
            color: #667085;
            font-size: 12px;
        }
        #settingsAccountValue, #settingsConnectionValue, #settingsSdkAppIdValue, #settingsSignatureValue {
            color: #172033;
            font-size: 14px;
            font-weight: 500;
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
            font-size: 16px;
            font-weight: 600;
        }
        #statusBadge {
            background: #e7f8ee;
            color: #087443;
            border-radius: 8px;
            padding: 4px 10px;
            font-size: 12px;
            font-weight: 500;
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
    connect(navSearchInput_, &QLineEdit::textChanged, this, [this] { applyConversationFilter(); });
    connect(messageNavButton_, &QPushButton::clicked, this, [this] { showMessagesPage(); });
    connect(contactsNavButton_, &QPushButton::clicked, this, [this] { showContactsPage(); });
    connect(settingsNavButton_, &QPushButton::clicked, this, [this] { showSettingsPage(); });
    connect(contentStack_, &QStackedWidget::currentChanged, this, [this] { syncNavigationSelection(); });
    connect(imageButton_, &QPushButton::clicked, this, [this] { openImagePicker(); });
    connect(voiceButton_, &QPushButton::clicked, this, [this] { app_.sendVoicePlaceholder(); });
    connect(sendButton_, &QPushButton::clicked, this, [this] { sendCurrentText(); });
    connect(messageEditor_, &QTextEdit::textChanged, this, [this] {
        updateComposerState();
        updateSlashCommandSuggestions();
    });
    conversationList_->installEventFilter(this);
    contactsList_->installEventFilter(this);
    conversationList_->setContextMenuPolicy(Qt::CustomContextMenu);
    contactsList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(conversationList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showContactContextMenu(conversationList_, pos);
    });
    connect(contactsList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showContactContextMenu(contactsList_, pos);
    });
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
    applyConversationFilter();
}

void MainWindow::applyConversationFilter() {
    if (!navSearchInput_) return;
    const QString needle = navSearchInput_->text().trimmed();
    for (int row = 0; row < conversationList_->count(); ++row) {
        QListWidgetItem* item = conversationList_->item(row);
        const QString name = item->data(DisplayNameRole).toString();
        const QString preview = item->data(PreviewRole).toString();
        const bool matched = needle.isEmpty()
            || name.contains(needle, Qt::CaseInsensitive)
            || preview.contains(needle, Qt::CaseInsensitive);
        item->setHidden(!matched);
    }
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
        const bool isSelected = button == selectedButton;
        button->setProperty("selected", isSelected);
        applyNavButtonIcon(button, isSelected);
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
        updateMessageBubbleWidths();
        messageScroll_->verticalScrollBar()->setValue(messageScroll_->verticalScrollBar()->maximum());
    });
}

void MainWindow::openAddContactDialog() {
    AddContactDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString userId = dialog.userId();
    if (userId.isEmpty()) return;
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
    if (viewportWidth <= 80) {
        // Viewport not laid out yet; estimate. Corrected by updateMessageBubbleWidths()
        // once the window is shown/resized, so the real viewport width is authoritative.
        viewportWidth = qMax(360, width() / 2);
    }
    // Leave room for the layout margins (28*2) plus the 42px gutter on the opposite
    // side of each bubble, so a row never exceeds the viewport (which would otherwise
    // clip content or force a horizontal scrollbar).
    return qBound(280, viewportWidth - 110, 1280);
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
    updateSlashCommandSuggestions();
}

void MainWindow::updateComposerState() {
    const bool hasPeer = !app_.chatState().selectedPeerId().isEmpty();
    const bool hasText = messageEditor_ && !messageEditor_->toPlainText().trimmed().isEmpty();
    imageButton_->setEnabled(hasPeer);
    messageEditor_->setEnabled(hasPeer);
    voiceButton_->setEnabled(hasPeer);
    sendButton_->setEnabled(hasPeer && hasText);
}

void MainWindow::updateSlashCommandSuggestions() {
    if (!slashCommandBar_ || !slashCommandLayout_ || !messageEditor_) return;

    while (QLayoutItem* item = slashCommandLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) delete widget;
        delete item;
    }

    const QString query = messageEditor_->toPlainText().trimmed();
    if (query.isEmpty() || !query.startsWith(QLatin1Char('/')) || app_.chatState().selectedPeerId().isEmpty()) {
        slashCommandBar_->setVisible(false);
        return;
    }

    bool hasMatch = false;
    for (const SlashCommandDefinition& definition : slashCommandDefinitions()) {
        if (!definition.command.startsWith(query, Qt::CaseInsensitive)) continue;
        auto* button = new QPushButton(definition.command + QStringLiteral("  ") + definition.label, slashCommandBar_);
        button->setObjectName(definition.objectName);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral(R"(
            QPushButton {
                min-height: 28px;
                border: 1px solid #b8def7;
                border-radius: 14px;
                background: #eff9ff;
                color: #0b67b7;
                padding: 0 12px;
                font-size: 12px;
                font-weight: 600;
                text-align: left;
            }
            QPushButton:hover {
                background: #dff1ff;
                border-color: #58b7ff;
            }
        )"));
        connect(button, &QPushButton::clicked, this, [this, command = definition.command] {
            selectSlashCommand(command);
        });
        slashCommandLayout_->addWidget(button, 0, Qt::AlignLeft);
        hasMatch = true;
    }

    if (hasMatch) {
        slashCommandLayout_->addStretch(1);
    }
    slashCommandBar_->setVisible(hasMatch);
}

void MainWindow::selectSlashCommand(const QString& command) {
    if (!messageEditor_) return;
    messageEditor_->setPlainText(command);
    QTextCursor cursor = messageEditor_->textCursor();
    cursor.movePosition(QTextCursor::End);
    messageEditor_->setTextCursor(cursor);
    messageEditor_->setFocus();
    updateComposerState();
    updateSlashCommandSuggestions();
}

void MainWindow::showContactContextMenu(QListWidget* list, const QPoint& pos) {
    if (!list) return;
    QListWidgetItem* item = list->itemAt(pos);
    if (!item) return;
    list->setCurrentItem(item);

    QMenu menu(this);
    QAction* deleteAction = menu.addAction(QStringLiteral("删除联系人"));
    QAction* selectedAction = menu.exec(list->viewport()->mapToGlobal(pos));
    if (selectedAction == deleteAction) {
        deleteContactFromItem(item);
    }
}

void MainWindow::deleteContactFromItem(QListWidgetItem* item) {
    if (!item) return;
    const QString userId = item->data(UserIdRole).toString().trimmed();
    if (userId.isEmpty()) return;
    app_.deleteContact(userId);
}

void MainWindow::deleteSelectedContactFromList(QListWidget* list) {
    if (!list) return;
    deleteContactFromItem(list->currentItem());
}

QString MainWindow::contactName(const QString& userId) const {
    for (const RemoteIMContact& contact : app_.chatState().contacts()) {
        if (contact.userId == userId) return contact.displayName.isEmpty() ? contact.userId : contact.displayName;
    }
    return userId;
}
