#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QApplication>
#include <QClipboard>
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>
#include <QColor>
#include <QFontMetrics>
#include <QTextBlock>
#include <QTextFormat>
#include <QTextFragment>
#include <QDialog>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
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
constexpr int UnreadRole = Qt::UserRole + 4;

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
        // 对齐 Electron .remote-im-markdown ul/ol 的 padding-left:20px——Qt 列表
        // 缩进来自 indentWidth（默认 40px 太深），CSS margin-left 对列表无效。
        document()->setIndentWidth(20);
        viewport()->setAutoFillBackground(false);
        // 对齐 Electron 端 .remote-im-bubble 正文：14px / #0f172a，链接 #2563eb。
        setStyleSheet(QStringLiteral(R"(
            QTextBrowser {
                color: #0f172a;
                background: transparent;
                border: 0;
                font-size: 14px;
            }
            QTextBrowser a {
                color: #2563eb;
            }
        )"));
    }

    void setMessageMarkdown(const QString& markdown) {
        setHtml(MarkdownRenderer::renderToHtml(markdown));
        // Qt 富文本 CSS 子集不支持 line-height，setHtml 后统一用块格式补上，
        // 对齐 Electron .remote-im-bubble 的 line-height:1.55（含代码块，两端一致）。
        QTextCursor cursor(document());
        cursor.select(QTextCursor::Document);
        QTextBlockFormat lineHeight;
        lineHeight.setLineHeight(155, QTextBlockFormat::ProportionalHeight);
        cursor.mergeBlockFormat(lineHeight);
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

        // 未读红点（钉钉/飞书风格）：预览行右侧画红色圆角计数徽标，99+ 封顶；
        // 打开会话即清零（ChatState::selectPeer），徽标随之消失。
        QRect clippedPreviewRect = previewRect;
        const int unread = index.data(UnreadRole).toInt();
        if (unread > 0) {
            const QString badgeText = unread > 99 ? QStringLiteral("99+") : QString::number(unread);
            QFont badgeFont = option.font;
            badgeFont.setPixelSize(11);
            badgeFont.setBold(true);
            const int badgeHeight = 18;
            const int badgeWidth = qMax(badgeHeight,
                                        QFontMetrics(badgeFont).horizontalAdvance(badgeText) + 10);
            const QRect badgeRect(rowRect.right() - badgeWidth - 10, previewRect.top(), badgeWidth, badgeHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#f53f3f")));
            painter->drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
            painter->setFont(badgeFont);
            painter->setPen(Qt::white);
            painter->drawText(badgeRect, Qt::AlignCenter, badgeText);
            clippedPreviewRect.setRight(badgeRect.left() - 8);
        }

        QFont previewFont = option.font;
        previewFont.setPixelSize(13);
        painter->setFont(previewFont);
        painter->setPen(QColor(QStringLiteral("#667085")));
        painter->drawText(clippedPreviewRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(previewFont).elidedText(preview, Qt::ElideRight, clippedPreviewRect.width()));

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

constexpr int kSlashCommandRowHeight = 32;

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
        {QStringLiteral("/model "), QStringLiteral("模型/推理"), QStringLiteral("slashCommandButton_model")},
        {QStringLiteral("/goal "), QStringLiteral("管理 Goal"), QStringLiteral("slashCommandButton_goal")},
        {QStringLiteral("/btw "), QStringLiteral("子任务"), QStringLiteral("slashCommandButton_btw")},
        {QStringLiteral("/diff "), QStringLiteral("仓库 Diff"), QStringLiteral("slashCommandButton_diff")},
        {QStringLiteral("/interrupt"), QStringLiteral("中断任务"), QStringLiteral("slashCommandButton_interrupt")},
        {QStringLiteral("/compact"), QStringLiteral("压缩上下文"), QStringLiteral("slashCommandButton_compact")},
        {QStringLiteral("/clear"), QStringLiteral("清空上下文"), QStringLiteral("slashCommandButton_clear")},
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

bool isHtmlFile(const RemoteIMFileAttachment& attachment) {
    const QString mimeType = attachment.mimeType.toLower();
    const QString fileName = attachment.fileName.toLower();
    return mimeType.contains(QStringLiteral("html"))
        || fileName.endsWith(QStringLiteral(".html"))
        || fileName.endsWith(QStringLiteral(".htm"));
}

QString readTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("文件暂不可预览");
    }
    return QString::fromUtf8(file.readAll());
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
        // Ctrl+V：剪贴板里有图片/文件则直接发送（消费按键）；否则交给默认粘贴文本。
        if (keyEvent->key() == Qt::Key_V
            && (keyEvent->modifiers() & Qt::ControlModifier)
            && !(keyEvent->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
            if (handleComposerPaste()) return true;
        }
    }
    if (watched == messageEditor_ && event->type() == QEvent::InputMethod) {
        // 输入法组词期间绝不重建命令提示条：组词从按下第一个拼音键开始，此刻若销毁/新建
        // 按钮、隐藏或抬升悬浮层，会打断编辑器的输入法上下文，导致首个拼音键被当作普通
        // 字符漏进输入框（如 /goal 后打 nihao 变成字面 n + 组词 ihao）。取消待执行的重建，
        // 组词结束（上屏或取消）后再刷新命令栏。事件不拦截，交给 QTextEdit 正常处理。
        auto* imeEvent = static_cast<QInputMethodEvent*>(event);
        const bool composing = !imeEvent->preeditString().isEmpty();
        if (composing) {
            imeComposing_ = true;
            if (slashCommandUpdateTimer_) slashCommandUpdateTimer_->stop();
        } else if (imeComposing_) {
            imeComposing_ = false;
            if (slashCommandUpdateTimer_) slashCommandUpdateTimer_->start();
        }
    }
    if (messageScroll_ && watched == messageScroll_->viewport() && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, [this] {
        updateMessageBubbleWidths();
        if (slashCommandBar_ && slashCommandBar_->isVisible()) positionSlashCommandBar();
    });
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
}

void MainWindow::buildUi() {
    // 单个空格而不是空串：空标题时 Qt 会回退显示 applicationDisplayName
    // （"MaiChat"），飞书风格的标题栏不显示文字。
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

    // 命令提示条：悬浮在输入框上方的纵向列表，不占 composer 布局空间。
    auto* slashCommandScroll = new QScrollArea(chatContentPane);
    slashCommandBar_ = slashCommandScroll;
    slashCommandBar_->setObjectName(QStringLiteral("slashCommandBar"));
    slashCommandBar_->setVisible(false);
    slashCommandScroll->setFrameShape(QFrame::NoFrame);
    slashCommandScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    slashCommandScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    slashCommandScroll->setWidgetResizable(true);
    slashCommandScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { border: 1px solid #dbe4ef; border-radius: 10px; background: #ffffff; }"));

    auto* slashCommandContent = new QWidget(slashCommandScroll);
    slashCommandContent->setObjectName(QStringLiteral("slashCommandContent"));
    slashCommandContent->setStyleSheet(QStringLiteral("#slashCommandContent { background: #ffffff; }"));
    slashCommandLayout_ = new QVBoxLayout(slashCommandContent);
    slashCommandLayout_->setContentsMargins(8, 8, 8, 8);
    slashCommandLayout_->setSpacing(4);
    slashCommandScroll->setWidget(slashCommandContent);

    messageEditor_ = new QTextEdit(composer);
    messageEditor_->setObjectName(QStringLiteral("messageEditor"));
    messageEditor_->setPlaceholderText(QStringLiteral("输入消息（可 Ctrl+V 粘贴图片或文件）"));
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

    sendButton_ = new QPushButton(QStringLiteral("发送"), composer);
    sendButton_->setObjectName(QStringLiteral("sendButton"));
    sendButton_->setCursor(Qt::PointingHandCursor);

    toolBar->addWidget(voiceButton_);
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
    connect(messageComposerSplitter, &QSplitter::splitterMoved, this, [this] {
        if (slashCommandBar_ && slashCommandBar_->isVisible()) positionSlashCommandBar();
    });

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
    connect(voiceButton_, &QPushButton::clicked, this, [this] { app_.sendVoicePlaceholder(); });
    connect(sendButton_, &QPushButton::clicked, this, [this] { sendCurrentText(); });
    // 命令提示条的重建（删除全部按钮、隐藏/抬升悬浮层）必须延后到事件循环下一轮，
    // 不能在 textChanged 里同步做——textChanged 是在 QTextEdit 的按键事件派发内部发出的，
    // 若此刻销毁 12 个按钮并隐藏被 raise() 的悬浮层，会吞掉紧随其后的 KeyRelease，
    // 让 Windows 认为按键仍按住而持续自动重复（输入 /g 变成 /gggggg……）。
    slashCommandUpdateTimer_ = new QTimer(this);
    slashCommandUpdateTimer_->setSingleShot(true);
    slashCommandUpdateTimer_->setInterval(150);  // 防抖：只在停顿后重建，避开按键前后那一瞬间
    connect(slashCommandUpdateTimer_, &QTimer::timeout, this, [this] { updateSlashCommandSuggestions(); });
    connect(messageEditor_, &QTextEdit::textChanged, this, [this] {
        updateComposerState();
        // 组词期间不触发重建（由 InputMethod 事件在组词结束时再拉起）；否则重启防抖定时器：
        // 连续输入天然合并成一次重建，且始终落在按键/组词之外。
        if (imeComposing_) return;
        slashCommandUpdateTimer_->start();
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
        if (!userId.isEmpty() && userId != app_.chatState().selectedPeerId()) {
            if (composerHasAttachments()) messageEditor_->clear();  // 丢弃属上一个会话的内联附件草稿
            app_.selectPeer(userId);
        }
    });
    auto openContactConversation = [this](QListWidgetItem* item) {
        if (!item) return;
        const QString userId = item->data(Qt::UserRole).toString();
        if (userId.isEmpty()) return;
        if (userId != app_.chatState().selectedPeerId() && composerHasAttachments()) messageEditor_->clear();
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
        item->setData(UnreadRole, app_.chatState().unreadCount(contact.userId));
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

    const QList<RemoteIMMessage> messages = app_.chatState().messagesWith(selectedPeer);
    bool needFullRebuild = selectedPeer != renderedPeerId_
        || renderedEmptyView_ != messages.isEmpty()
        || messageLayout_->count() == 0;
    if (!needFullRebuild && renderedMessageIds_.size() == messages.size()) {
        QStringList nextIds;
        nextIds.reserve(messages.size());
        for (const RemoteIMMessage& message : messages) nextIds.append(message.id);
        if (nextIds != renderedMessageIds_) {
            QSet<QString> renderedIds;
            QSet<QString> nextIdSet;
            for (const QString& id : renderedMessageIds_) renderedIds.insert(id);
            for (const QString& id : nextIds) nextIdSet.insert(id);
            // 漫游记录为旧消息补齐规范化时间后，同一批消息可能需要原位重排。
            // 增删仍走增量路径；仅集合相同但顺序变化时完整重建。
            needFullRebuild = renderedIds == nextIdSet;
        }
    }
    if (needFullRebuild) {
        rebuildMessageList(selectedPeer, messages);
        return;
    }
    applyIncrementalMessageUpdate(messages);
}

void MainWindow::rebuildMessageList(const QString& peerId, const QList<RemoteIMMessage>& messages) {
    while (QLayoutItem* item = messageLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) delete widget;
        delete item;
    }
    renderedPeerId_ = peerId;
    renderedMessageIds_.clear();
    messageRowById_.clear();
    renderedStatusById_.clear();
    loadEarlierButton_ = nullptr;
    renderedEmptyView_ = messages.isEmpty();

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
        return;
    }

    // 布局固定结构：[0]=加载更早按钮（无更早时隐藏），随后消息行，末尾弹簧。
    loadEarlierButton_ = new QPushButton(QStringLiteral("加载更早的消息"), messageContainer_);
    loadEarlierButton_->setObjectName(QStringLiteral("loadEarlierButton"));
    loadEarlierButton_->setCursor(Qt::PointingHandCursor);
    loadEarlierButton_->setStyleSheet(QStringLiteral(R"(
        QPushButton#loadEarlierButton {
            background: #f1f5f9;
            border: 1px solid #e2e8f0;
            border-radius: 14px;
            color: #475569;
            font-size: 12px;
            font-weight: 700;
            padding: 5px 14px;
        }
        QPushButton#loadEarlierButton:hover {
            background: #e2e8f0;
        }
    )"));
    connect(loadEarlierButton_, &QPushButton::clicked, this, [this] {
        app_.loadEarlierMessages(app_.chatState().selectedPeerId());
    });
    auto* buttonRow = new QWidget(messageContainer_);
    auto* buttonRowLayout = new QHBoxLayout(buttonRow);
    buttonRowLayout->setContentsMargins(0, 0, 0, 0);
    buttonRowLayout->addStretch(1);
    buttonRowLayout->addWidget(loadEarlierButton_);
    buttonRowLayout->addStretch(1);
    messageLayout_->addWidget(buttonRow);

    for (const RemoteIMMessage& message : messages) {
        QWidget* row = createMessageBubble(message);
        messageLayout_->addWidget(row);
        renderedMessageIds_.append(message.id);
        messageRowById_.insert(message.id, row);
        renderedStatusById_.insert(message.id, message.status);
    }
    messageLayout_->addStretch(1);
    updateLoadEarlierVisibility();

    QTimer::singleShot(0, this, [this] {
        updateMessageBubbleWidths();
        scrollMessagesToBottom();
    });
}

void MainWindow::applyIncrementalMessageUpdate(const QList<RemoteIMMessage>& messages) {
    QSet<QString> newIds;
    newIds.reserve(messages.size());
    for (const RemoteIMMessage& message : messages) newIds.insert(message.id);

    // 移除已消失的消息（如临时 UUID 被 SDK 稳定 id 采纳后旧行退场）。
    for (const QString& id : renderedMessageIds_) {
        if (newIds.contains(id)) continue;
        if (QWidget* row = messageRowById_.take(id)) {
            messageLayout_->removeWidget(row);
            row->deleteLater();
        }
        renderedStatusById_.remove(id);
    }

    // 首个仍在的旧消息在新列表中的位置：其前方的新增视为「向上翻页」，
    // 其后方的新增视为实时追加。
    int firstKeptIndex = messages.size();
    for (int i = 0; i < messages.size(); ++i) {
        if (messageRowById_.contains(messages.at(i).id)) {
            firstKeptIndex = i;
            break;
        }
    }

    QScrollBar* bar = messageScroll_->verticalScrollBar();
    const int oldMax = bar->maximum();
    const int oldValue = bar->value();
    const bool wasNearBottom = oldValue >= oldMax - 60;

    bool prepended = false;
    bool appended = false;
    constexpr int kLayoutBase = 1;  // [0] 是加载更早按钮行
    QStringList resultIds;
    resultIds.reserve(messages.size());
    for (int i = 0; i < messages.size(); ++i) {
        const RemoteIMMessage& message = messages.at(i);
        resultIds.append(message.id);
        if (QWidget* existing = messageRowById_.value(message.id)) {
            if (renderedStatusById_.value(message.id) != message.status) {
                // 状态徽标在气泡内部：原位替换单个气泡，代价 O(1)。
                const int layoutIndex = messageLayout_->indexOf(existing);
                QWidget* fresh = createMessageBubble(message);
                messageLayout_->removeWidget(existing);
                existing->deleteLater();
                messageLayout_->insertWidget(layoutIndex, fresh);
                messageRowById_.insert(message.id, fresh);
                renderedStatusById_.insert(message.id, message.status);
            }
            continue;
        }
        QWidget* row = createMessageBubble(message);
        messageLayout_->insertWidget(kLayoutBase + i, row);
        messageRowById_.insert(message.id, row);
        renderedStatusById_.insert(message.id, message.status);
        if (i < firstKeptIndex) prepended = true;
        else appended = true;
    }
    renderedMessageIds_ = resultIds;
    updateLoadEarlierVisibility();

    QTimer::singleShot(0, this, [this, prepended, appended, wasNearBottom, oldMax, oldValue] {
        updateMessageBubbleWidths();
        QScrollBar* bar = messageScroll_->verticalScrollBar();
        QObject::disconnect(messageScrollToBottomConn_);
        if (prepended) {
            // 向上翻页：锚定原可视位置（新内容顶入的高度差补偿到滚动值）。
            messageScrollToBottomConn_ = connect(
                bar, &QAbstractSlider::rangeChanged, this,
                [this, bar, oldMax, oldValue](int, int max) {
                    bar->setValue(oldValue + (max - oldMax));
                    QObject::disconnect(messageScrollToBottomConn_);
                });
            bar->setValue(oldValue + (bar->maximum() - oldMax));
            return;
        }
        if (appended && wasNearBottom) {
            scrollMessagesToBottom();
        }
    });
}

void MainWindow::updateLoadEarlierVisibility() {
    if (!loadEarlierButton_) return;
    loadEarlierButton_->setVisible(app_.hasEarlierMessages(app_.chatState().selectedPeerId()));
}

void MainWindow::scrollMessagesToBottom() {
    // 气泡高度依赖刚设好的宽度（自动换行），滚动条范围要到下一轮布局才正确；
    // 此刻直接读 maximum() 常拿到旧值，改为等 rangeChanged 再跳到底，一次性触发；
    // 先断开上一次挂起的连接，避免快速切换会话时处理器堆叠。
    QScrollBar* bar = messageScroll_->verticalScrollBar();
    QObject::disconnect(messageScrollToBottomConn_);
    messageScrollToBottomConn_ = connect(
        bar, &QAbstractSlider::rangeChanged, this, [this, bar](int, int max) {
            bar->setValue(max);
            QObject::disconnect(messageScrollToBottomConn_);
        });
    // 内容本就放得下、不会触发 rangeChanged 时的兜底：此时 maximum() 已正确。
    bar->setValue(bar->maximum());
}

void MainWindow::openAddContactDialog() {
    AddContactDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString userId = dialog.userId();
    if (userId.isEmpty()) return;
    app_.addContact(userId, userId);
}

bool MainWindow::handleComposerPaste() {
    if (app_.chatState().selectedPeerId().isEmpty()) return false;
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime) return false;

    // 1) 剪贴板里的本地文件（资源管理器复制的文件）：内联插入到输入框。
    if (mime->hasUrls()) {
        QStringList files;
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            if (QFileInfo(path).isFile()) files << path;
        }
        if (!files.isEmpty()) {
            for (const QString& path : files) insertComposerFile(path);
            return true;
        }
    }

    // 2) 剪贴板里的图像数据（截图工具、复制的图片）：内联插入到输入框。
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            insertComposerImage(image);
            return true;
        }
    }
    return false;  // 交给 QTextEdit 默认粘贴（文本）
}

void MainWindow::insertComposerImage(const QImage& image) {
    // 原图存临时 PNG（发送用原图）；内联显示时按最大宽度缩放，资源名即文件路径，
    // QTextEdit 会从磁盘加载渲染，发送时也从这个路径取原图。
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                            .filePath(QStringLiteral("maichat-paste"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(
        QStringLiteral("paste-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!image.save(path, "PNG")) return;

    QTextImageFormat fmt;
    fmt.setName(path);
    int w = image.width();
    int h = image.height();
    constexpr int kMaxWidth = 240;
    if (w > kMaxWidth && w > 0) {
        h = h * kMaxWidth / w;
        w = kMaxWidth;
    }
    fmt.setWidth(w);
    fmt.setHeight(h);
    QTextCursor cursor = messageEditor_->textCursor();
    cursor.insertImage(fmt);
    messageEditor_->setTextCursor(cursor);
    messageEditor_->setFocus();
    updateComposerState();
}

void MainWindow::insertComposerFile(const QString& localPath) {
    // 文件在输入框里用一枚「文件卡」缩略图表示（📄 文件名），资源名带 pending-file:// 前缀，
    // 发送时据此识别为文件（区别于图片路径）。
    QString shown = QFileInfo(localPath).fileName();
    if (shown.size() > 22) shown = shown.left(19) + QStringLiteral("…");
    const QString label = QStringLiteral("📄 ") + shown;

    const QFontMetrics fm(messageEditor_->font());
    const int chipW = fm.horizontalAdvance(label) + 22;
    const int chipH = 28;
    QPixmap chip(chipW, chipH);
    chip.fill(Qt::transparent);
    {
        QPainter p(&chip);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(QStringLiteral("#d9e4ef"))));
        p.setBrush(QColor(QStringLiteral("#f1f6fb")));
        p.drawRoundedRect(QRectF(0.5, 0.5, chipW - 1.0, chipH - 1.0), 6, 6);
        p.setPen(QColor(QStringLiteral("#33475b")));
        p.drawText(QRectF(11, 0, chipW - 14.0, chipH), Qt::AlignVCenter | Qt::AlignLeft, label);
    }

    const QString resourceName = QStringLiteral("pending-file://") + localPath;
    messageEditor_->document()->addResource(QTextDocument::ImageResource, QUrl(resourceName), chip);
    QTextImageFormat fmt;
    fmt.setName(resourceName);
    fmt.setWidth(chipW);
    fmt.setHeight(chipH);
    QTextCursor cursor = messageEditor_->textCursor();
    cursor.insertImage(fmt);
    messageEditor_->setTextCursor(cursor);
    messageEditor_->setFocus();
    updateComposerState();
}

bool MainWindow::composerHasAttachments() const {
    // 内联的图片/文件在纯文本里表现为对象替换符（U+FFFC）。
    return messageEditor_ && messageEditor_->toPlainText().contains(QChar(0xFFFC));
}

QList<MainWindow::ComposerAttachment> MainWindow::collectComposerAttachments() const {
    QList<ComposerAttachment> attachments;
    if (!messageEditor_) return attachments;
    const QString filePrefix = QStringLiteral("pending-file://");
    const QTextDocument* doc = messageEditor_->document();
    // 按文档顺序取出所有内联对象（图片/文件）。
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isImageFormat()) continue;
            const QString name = frag.charFormat().toImageFormat().name();
            if (name.startsWith(filePrefix)) {
                attachments.append(ComposerAttachment{true, name.mid(filePrefix.size())});
            } else {
                attachments.append(ComposerAttachment{false, name});
            }
        }
    }
    return attachments;
}

void MainWindow::openImagePreview(const QString& imagePath) {
    ImagePreviewDialog dialog(imagePath, this);
    dialog.showFullScreen();
    dialog.exec();
}

void MainWindow::openFilePreview(const RemoteIMFileAttachment& attachment) {
    QDialog dialog(this);
    const QString displayName = attachment.fileName.isEmpty() ? QFileInfo(attachment.localPath).fileName() : attachment.fileName;
    dialog.setWindowTitle(displayName.isEmpty() ? QStringLiteral("文件预览") : displayName);
    dialog.resize(qMax(720, width() / 2), qMax(520, height() / 2));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto* title = new QLabel(dialog.windowTitle(), &dialog);
    title->setObjectName(QStringLiteral("filePreviewTitle"));
    layout->addWidget(title);

    auto* preview = new QTextBrowser(&dialog);
    preview->setObjectName(QStringLiteral("filePreviewContent"));
    preview->setOpenExternalLinks(true);
    preview->setReadOnly(true);
    if (isHtmlFile(attachment)) {
        preview->setHtml(readTextFile(attachment.localPath));
    } else {
        preview->setHtml(MarkdownRenderer::renderToHtml(readTextFile(attachment.localPath)));
    }
    layout->addWidget(preview, 1);

    auto* closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(closeButton, 0, Qt::AlignRight);

    dialog.setStyleSheet(QStringLiteral(R"(
        QLabel#filePreviewTitle {
            color: #101828;
            font-size: 18px;
            font-weight: 800;
        }
        QTextBrowser#filePreviewContent {
            border: 1px solid #d9e4ef;
            border-radius: 8px;
            padding: 12px;
            background: #ffffff;
            color: #172033;
            font-size: 14px;
        }
    )"));
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
    const bool expandedTextBubble = !message.hasImage && !message.hasFile && (message.text.size() >= 50 || message.text.contains(QLatin1Char('\n')));
    bubble->setProperty("expandedTextBubble", expandedTextBubble);
    applyMessageBubbleWidth(bubble, expandedTextBubble);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // 配色/圆角对齐 Electron 端 .remote-im-bubble：本方(用户)白底 #dbeafe 边，
    // 对方(aicli)米黄底 #fffbeb + #fde68a 边，圆角 16px。
    bubble->setStyleSheet(outgoing
                              ? QStringLiteral("#messageBubbleOutgoing{background:#ffffff;border:1px solid #dbeafe;border-radius:16px;}")
                              : QStringLiteral("#messageBubbleIncoming{background:#fffbeb;border:1px solid #fde68a;border-radius:16px;}"));

    auto* bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(14, 11, 14, 12);
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
    } else if (message.hasFile) {
        auto* fileButton = new QPushButton(bubble);
        fileButton->setObjectName(QStringLiteral("messageFileButton"));
        fileButton->setCursor(Qt::PointingHandCursor);
        const QString displayName = message.file.fileName.isEmpty()
            ? QFileInfo(message.file.localPath).fileName()
            : message.file.fileName;
        fileButton->setText(QStringLiteral("📄 %1\n%2")
            .arg(displayName.isEmpty() ? QStringLiteral("file") : displayName)
            .arg(isHtmlFile(message.file) ? QStringLiteral("HTML 文件，点击预览") : QStringLiteral("Markdown 文件，点击预览")));
        fileButton->setMinimumWidth(220);
        fileButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        fileButton->setStyleSheet(QStringLiteral(R"(
            QPushButton#messageFileButton {
                background: #f8fafc;
                border: 1px solid #d9e4ef;
                border-radius: 8px;
                color: #172033;
                font-size: 13px;
                font-weight: 700;
                padding: 10px 12px;
                text-align: left;
            }
            QPushButton#messageFileButton:hover {
                border-color: #1aa7ec;
                background: #edf8ff;
            }
        )"));
        connect(fileButton, &QPushButton::clicked, this, [this, attachment = message.file]() {
            openFilePreview(attachment);
        });
        contentRow->addWidget(fileButton);
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

    // 图片/文件带配文时：配文渲染在附件下方，与附件同属一条气泡（微信式图上文下）。
    // 占位文字（[图片消息]/[文件消息] …）不是真正配文，不再重复展示。
    if ((message.hasImage || message.hasFile)
            && !message.text.trimmed().isEmpty()
            && !message.text.startsWith(QStringLiteral("[图片消息] "))
            && !message.text.startsWith(QStringLiteral("[文件消息] "))) {
        auto* captionView = new MarkdownMessageView(bubble);
        captionView->setMessageMarkdown(message.text);
        bubbleLayout->addWidget(captionView);
    }
    // meta 行对齐 Electron 端 .remote-im-message-meta：作者 #334155/700、
    // 时间 #94a3b8、好友徽章 #ecfdf5 底 #047857 字 11px 胶囊。
    bubble->setStyleSheet(bubble->styleSheet() + QStringLiteral(R"(
        #messageAuthorLabel {
            color: #334155;
            font-size: 13px;
            font-weight: 700;
            background: transparent;
        }
        #messageTimeLabel {
            color: #94a3b8;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        #messageRelationBadge {
            background: #ecfdf5;
            border: 0;
            border-radius: 9px;
            color: #047857;
            padding: 2px 8px;
            font-size: 11px;
            font-weight: 800;
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
    if (app_.chatState().selectedPeerId().isEmpty()) return;
    QString text = messageEditor_->toPlainText();
    text.remove(QChar(0xFFFC));  // 去掉内联图片/文件的对象替换占位符
    text = text.trimmed();
    const QList<ComposerAttachment> attachments = collectComposerAttachments();
    if (text.isEmpty() && attachments.isEmpty()) return;

    if (attachments.isEmpty()) {
        app_.sendText(text);
    } else {
        // 文字并入「第一个」附件，合并成一条消息发送（气泡内图上文下）；其余附件各自单独发。
        for (int i = 0; i < attachments.size(); ++i) {
            const QString caption = (i == 0) ? text : QString();
            if (attachments.at(i).isFile) {
                app_.sendFile(attachments.at(i).path, caption);
            } else {
                app_.sendImage(attachments.at(i).path, caption);
            }
        }
    }

    messageEditor_->clear();
    updateComposerState();
    // 同样延后重建：sendCurrentText 可能由 Enter 键在事件过滤器里触发，走的是按键派发路径，
    // 不能在这里同步销毁按钮/隐藏悬浮层（否则会吞掉 Enter 的 KeyRelease）。
    slashCommandUpdateTimer_->start();
}

void MainWindow::updateComposerState() {
    const bool hasPeer = !app_.chatState().selectedPeerId().isEmpty();
    QString plain = messageEditor_ ? messageEditor_->toPlainText() : QString();
    plain.remove(QChar(0xFFFC));
    const bool hasText = !plain.trimmed().isEmpty();
    const bool hasAttachments = composerHasAttachments();
    messageEditor_->setEnabled(hasPeer);
    voiceButton_->setEnabled(hasPeer);
    sendButton_->setEnabled(hasPeer && (hasText || hasAttachments));
}

void MainWindow::updateSlashCommandSuggestions() {
    if (!slashCommandBar_ || !slashCommandLayout_ || !messageEditor_) return;
    if (imeComposing_) return;  // 组词进行中，绝不动命令栏控件，避免打断输入法

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
        QWidget* commandContent = qobject_cast<QWidget*>(slashCommandLayout_->parentWidget());
        auto* button = new QPushButton(definition.command + QStringLiteral("  ") + definition.label, commandContent ? commandContent : slashCommandBar_);
        button->setObjectName(definition.objectName);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFixedHeight(kSlashCommandRowHeight);
        button->setStyleSheet(QStringLiteral(R"(
            QPushButton {
                border: 1px solid #b8def7;
                border-radius: 8px;
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
        slashCommandLayout_->addWidget(button);
        hasMatch = true;
    }

    if (hasMatch) {
        positionSlashCommandBar();
        slashCommandBar_->raise();
    }
    slashCommandBar_->setVisible(hasMatch);
}

void MainWindow::positionSlashCommandBar() {
    if (!slashCommandBar_ || !slashCommandLayout_ || !messageEditor_) return;
    QWidget* overlayParent = slashCommandBar_->parentWidget();
    if (!overlayParent) return;

    // 内容高度按行数直接推算（按钮定高），不依赖布局 sizeHint 的刷新时机；
    // 最多显示 kMaxVisibleRows 行，更多时转纵向滚动，再按输入框上方的可用空间收缩。
    const int rowCount = slashCommandLayout_->count();
    if (rowCount <= 0) return;
    constexpr int kMaxVisibleRows = 10;
    const int visibleRows = qMin(rowCount, kMaxVisibleRows);
    const QMargins margins = slashCommandLayout_->contentsMargins();
    const int barHeightForRows = visibleRows * kSlashCommandRowHeight
        + (visibleRows - 1) * slashCommandLayout_->spacing()
        + margins.top() + margins.bottom() + 2;
    const QPoint editorTopLeft = messageEditor_->mapTo(overlayParent, QPoint(0, 0));
    int barHeight = barHeightForRows;
    barHeight = qMin(barHeight, qMax(60, editorTopLeft.y() - 16));
    const int barWidth = messageEditor_->width();
    slashCommandBar_->setGeometry(editorTopLeft.x(), editorTopLeft.y() - barHeight - 8, barWidth, barHeight);
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
    QAction* deleteAction = menu.addAction(QStringLiteral("删除好友及聊天历史"));
    QAction* selectedAction = menu.exec(list->viewport()->mapToGlobal(pos));
    if (selectedAction == deleteAction) {
        deleteContactFromItem(item);
    }
}

void MainWindow::deleteContactFromItem(QListWidgetItem* item) {
    if (!item) return;
    const QString userId = item->data(UserIdRole).toString().trimmed();
    if (userId.isEmpty()) return;
    const QString displayName = item->data(DisplayNameRole).toString().trimmed();
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this,
        QStringLiteral("删除好友"),
        QStringLiteral("确定删除好友“%1”及全部聊天历史吗？此操作不可恢复。")
            .arg(displayName.isEmpty() ? userId : displayName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes) return;
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
