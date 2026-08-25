#include "ui/MainWindow.h"
#include "model/MessageSearch.h"
#include "ui/MessageImageLoader.h"

#include <QCoreApplication>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QAction>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>
#include <QColor>
#include <QFontMetrics>
#include <QTextBlock>
#include <QTextFormat>
#include <QTextFragment>
#include <QDesktopServices>
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
#include <QShortcut>
#include <QMenu>
#include <QPixmap>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygon>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
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
#include <algorithm>
#include <functional>
#include <utility>

#include "im/RemoteIMCredentialDefaults.h"
#include "im/VideoFileMetadata.h"
#include "markdown/MarkdownRenderer.h"
#include "ui/AddContactDialog.h"
#include "ui/AppMessageDialog.h"
#include "ui/AppTextInputDialog.h"
#include "ui/FilePreviewDialog.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/VideoPreviewDialog.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QRadioButton>
#include <QRegularExpression>

#include "im/RemoteIMCredentialDefaults.h"
#include "im/TencentUserSigGenerator.h"
#include "remote/TrtcEngine.h"
#include "ui/RemoteDesktopConsentDialog.h"
#include "ui/RemoteDesktopProxyDialog.h"
#include "ui/RemoteDesktopSessionCard.h"
#include "ui/RemoteDesktopViewPanel.h"
#include "ui/SharingIndicatorBar.h"
#include "ui/UiZoom.h"

namespace {

constexpr int UserIdRole = Qt::UserRole;
constexpr int DisplayNameRole = Qt::UserRole + 1;
constexpr int PreviewRole = Qt::UserRole + 2;
constexpr int TimeRole = Qt::UserRole + 3;
constexpr int UnreadRole = Qt::UserRole + 4;
constexpr int AvatarUrlRole = Qt::UserRole + 5;
// 顶栏搜索结果项：记住它属于哪个会话、哪条消息，点开才能跳过去。
constexpr int SearchPeerRole = Qt::UserRole + 6;
constexpr int SearchMessageRole = Qt::UserRole + 7;
// 结果太多时列表本身就没用了，截断并提示收窄关键词。
constexpr int MaxGlobalSearchResults = 60;
// 导航栏与会话栏头部的图标统一用这个尺寸：混用会让一列图标看起来大小不一。
constexpr int kNavIconPixels = 20;
constexpr int MessageAvatarLogicalSize = 40;
constexpr int MessageAvatarGap = 10;
constexpr int MessageMetaBubbleGap = 6;
constexpr int RemoteDesktopStopSendTimeoutMs = 800;

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
        setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            QTextBrowser {
                color: #0f172a;
                background: transparent;
                border: 0;
                font-size: 14px;
            }
            QTextBrowser a {
                color: #2563eb;
            }
        )")));

        copyOriginalDataAction_ = new QAction(QStringLiteral("复制原始数据"), this);
        copyOriginalDataAction_->setObjectName(QStringLiteral("copyOriginalDataAction"));
        connect(copyOriginalDataAction_, &QAction::triggered, this, [this]() {
            QApplication::clipboard()->setText(sourceMarkdown_);
        });
    }

    void setMessageMarkdown(const QString& markdown) {
        // QTextDocument 只保留 Markdown 渲染后的富文本，无法无损还原标题、代码围栏、
        // 链接目标等源语法；单独保存传入的规范化原文供“复制原始数据”使用。
        sourceMarkdown_ = markdown;
        // 渲染器输出的 HTML 内嵌固定 px 字号（正文 14px/h1 22px/code 13px…），
        // 会盖过控件字体——整体缩放时须把这些 px 一并按倍率缩放。
        setHtml(UiZoom::scaleQss(MarkdownRenderer::renderToHtml(markdown)));
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

    // 替换 QTextBrowser 原生英文右键菜单（Copy/Copy Link Location/Select All），
    // 换成与图片/文件气泡一致的飞书式中文菜单。定义在辅助函数之后（见文件下方）。
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QString sourceMarkdown_;
    QAction* copyOriginalDataAction_ = nullptr;

    void updateContentHeight() {
        const int width = qMax(120, viewport()->width());
        if (!qFuzzyCompare(document()->textWidth(), static_cast<qreal>(width))) {
            document()->setTextWidth(width);
        }
        setFixedHeight(qMax(24, qCeil(document()->size().height()) + 2));
        updateGeometry();
    }
};

// 应用图标同款品牌渐变（MaiChat/brand/maichat-icon.svg：#5B9BFF → #1E40AF 对角），
// 头像等品牌色块统一用它，保持与桌面图标一个色系。
QBrush brandAvatarBrush(const QRectF& rect) {
    QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
    gradient.setColorAt(0.0, QColor(QStringLiteral("#5B9BFF")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#1E40AF")));
    return QBrush(gradient);
}

QString avatarMonogram(const QString& displayName, const QString& userId) {
    const QString cleanUserId = userId.trimmed();
    const QString cleanDisplayName = displayName.trimmed();
    const bool hasNickname = !cleanDisplayName.isEmpty() && cleanDisplayName != cleanUserId;
    const QString source = hasNickname ? cleanDisplayName : cleanUserId;
    if (source.isEmpty()) return QStringLiteral("M");
    if (!hasNickname) return source.left(1).toUpper();

    QString separated = source;
    separated.replace(QLatin1Char('-'), QLatin1Char(' '));
    separated.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QStringList words = separated.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() >= 2) {
        return (words.first().left(1) + words.last().left(1)).toUpper();
    }

    bool hasNonAscii = false;
    for (const QChar ch : source) {
        if (ch.unicode() > 0x7f) {
            hasNonAscii = true;
            break;
        }
    }
    return (hasNonAscii ? source.right(2) : source.left(2)).toUpper();
}

// 白色 monogram 字母压在深色头像块上时，ClearType 亚像素渲染的粉/青彩边在
// 纯色底上非常显眼，小字号下字形显脏（QFont::NoSubpixelAntialias 在 Windows
// 字体引擎上并不可靠）。整块头像改为离屏生成：文字走 QPainterPath 填充
// （纯灰度抗锯齿），按 DPR 物理分辨率渲染并缓存，导航/列表/消息区共用。
QPixmap monogramAvatarPixmap(const QString& text,
                             int logicalSize,
                             qreal radius,
                             const QColor& gradientFrom,
                             const QColor& gradientTo,
                             int fontPixelSize,
                             qreal dpr) {
    const QString key = QStringLiteral("monogram:%1:%2:%3:%4:%5:%6:%7")
                            .arg(text)
                            .arg(logicalSize)
                            .arg(radius)
                            .arg(gradientFrom.name())
                            .arg(gradientTo.name())
                            .arg(fontPixelSize)
                            .arg(dpr);
    static QHash<QString, QPixmap> cache;
    const auto found = cache.constFind(key);
    if (found != cache.cend()) return found.value();

    const int physical = qMax(1, qRound(logicalSize * dpr));
    QPixmap pixmap(physical, physical);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient gradient(0, 0, logicalSize, logicalSize);
    gradient.setColorAt(0, gradientFrom);
    gradient.setColorAt(1, gradientTo);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(0, 0, logicalSize, logicalSize), radius, radius);

    QFont font = QApplication::font();
    font.setPixelSize(fontPixelSize);
    font.setBold(true);
    const QFontMetricsF metrics(font);
    QPainterPath textPath;
    textPath.addText((logicalSize - metrics.horizontalAdvance(text)) / 2.0,
                     (logicalSize + metrics.ascent() - metrics.descent()) / 2.0,
                     font, text);
    painter.fillPath(textPath, Qt::white);
    painter.end();
    cache.insert(key, pixmap);
    return pixmap;
}

const QColor kBrandGradientFrom(0x5b, 0x9b, 0xff);
const QColor kBrandGradientTo(0x1e, 0x40, 0xaf);
const QColor kPeerGradientFrom(0x2d, 0xd4, 0xbf);
const QColor kPeerGradientTo(0x0f, 0x76, 0x6e);

QHash<QString, QPixmap>& avatarPixmapCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

QHash<QString, QList<QPointer<QWidget>>>& avatarRepaintWaiters() {
    static QHash<QString, QList<QPointer<QWidget>>> waiters;
    return waiters;
}

QSet<QString>& pendingAvatarUrls() {
    static QSet<QString> urls;
    return urls;
}

QSet<QString>& failedAvatarUrls() {
    static QSet<QString> urls;
    return urls;
}

void requestAvatarPixmap(const QString& avatarUrl, QWidget* repaintTarget) {
    const QString url = avatarUrl.trimmed();
    if (url.isEmpty() || avatarPixmapCache().contains(url) || failedAvatarUrls().contains(url)) return;

    if (repaintTarget) {
        QList<QPointer<QWidget>>& waiters = avatarRepaintWaiters()[url];
        const auto alreadyWaiting = std::any_of(
            waiters.cbegin(), waiters.cend(), [repaintTarget](const QPointer<QWidget>& item) {
                return item.data() == repaintTarget;
            });
        if (!alreadyWaiting) waiters.append(QPointer<QWidget>(repaintTarget));
    }
    if (pendingAvatarUrls().contains(url)) return;
    pendingAvatarUrls().insert(url);

    static QNetworkAccessManager* network = new QNetworkAccessManager(qApp);
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, url] {
        QPixmap source;
        if (reply->error() == QNetworkReply::NoError) source.loadFromData(reply->readAll());
        reply->deleteLater();

        pendingAvatarUrls().remove(url);
        if (source.isNull()) {
            failedAvatarUrls().insert(url);
        } else {
            avatarPixmapCache().insert(url, source);
        }
        const QList<QPointer<QWidget>> targets = avatarRepaintWaiters().take(url);
        for (const QPointer<QWidget>& target : targets) {
            if (!target.isNull()) target->update();
        }
    });
}

void drawAvatarPixmap(QPainter* painter, const QRectF& target, const QPixmap& source, qreal radius) {
    if (!painter || source.isNull()) return;
    // 高分屏（DPR>1）下必须按物理分辨率缩放并声明 devicePixelRatio：
    // 按逻辑尺寸缩放的位图会被绘制层再放大一次，头像整体发虚。
    // 文字与 QSS 是矢量渲染不受影响，位图需要自己处理。
    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    QPixmap scaled = source.scaled((target.size() * dpr).toSize(),
                                   Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    const QSizeF logicalSize = QSizeF(scaled.size()) / dpr;
    const QPointF topLeft(target.center().x() - logicalSize.width() / 2.0,
                          target.center().y() - logicalSize.height() / 2.0);
    QPainterPath clip;
    clip.addRoundedRect(target, radius, radius);
    painter->save();
    painter->setClipPath(clip);
    painter->drawPixmap(topLeft, scaled);
    painter->restore();
}

bool drawRemoteAvatar(QPainter* painter,
                      const QRectF& target,
                      const QString& avatarUrl,
                      qreal radius,
                      QWidget* repaintTarget) {
    const QString url = avatarUrl.trimmed();
    const auto cached = avatarPixmapCache().constFind(url);
    if (cached != avatarPixmapCache().cend()) {
        drawAvatarPixmap(painter, target, cached.value(), radius);
        return true;
    }
    requestAvatarPixmap(url, repaintTarget);
    return false;
}

class MessageAvatarLabel final : public QLabel {
public:
    MessageAvatarLabel(QString avatarUrl, bool outgoing, QWidget* parent)
        : QLabel(parent), avatarUrl_(std::move(avatarUrl)), outgoing_(outgoing) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto cached = avatarPixmapCache().constFind(avatarUrl_);
        if (cached != avatarPixmapCache().cend()) {
            drawAvatarPixmap(&painter, rect(), cached.value(), UiZoom::s(10));
            return;
        }
        // 头像未下载（或没有头像）时显示生成的 monogram 块。
        painter.drawPixmap(0, 0,
                           monogramAvatarPixmap(text(), width(), UiZoom::s(10),
                                                outgoing_ ? kBrandGradientFrom : kPeerGradientFrom,
                                                outgoing_ ? kBrandGradientTo : kPeerGradientTo,
                                                UiZoom::s(12), devicePixelRatioF()));
        requestAvatarPixmap(avatarUrl_, this);
    }

private:
    QString avatarUrl_;
    bool outgoing_ = false;
};

QLabel* createMessageAvatarLabel(const QString& userId,
                                 const QString& displayName,
                                 const QString& avatarUrl,
                                 bool outgoing,
                                 QWidget* parent) {
    auto* avatar = new MessageAvatarLabel(avatarUrl.trimmed(), outgoing, parent);
    avatar->setText(avatarMonogram(displayName, userId));
    avatar->setObjectName(outgoing ? QStringLiteral("messageAvatarOutgoing")
                                   : QStringLiteral("messageAvatarIncoming"));
    avatar->setProperty("avatarUserId", userId);
    avatar->setProperty("avatarDisplayName", displayName);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(UiZoom::s(MessageAvatarLogicalSize),
                         UiZoom::s(MessageAvatarLogicalSize));
    avatar->setToolTip(displayName == userId || displayName.trimmed().isEmpty()
                           ? userId
                           : QStringLiteral("%1 (%2)").arg(displayName, userId));
    avatar->setAccessibleName(QStringLiteral("%1 的头像").arg(displayName.isEmpty() ? userId : displayName));
    // 背景/字母全部由 paintEvent 的生成位图绘制（QPainterPath 灰度抗锯齿、
    // 无浅色描边光晕），不再使用 QSS 背景。
    avatar->setProperty("avatarUrl", avatarUrl.trimmed());
    return avatar;
}

class ConversationListDelegate final : public QStyledItemDelegate {
public:
    explicit ConversationListDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, UiZoom::s(72));
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

        const QString userId = index.data(UserIdRole).toString();
        const QString name = index.data(DisplayNameRole).toString();
        const QString avatarUrl = index.data(AvatarUrlRole).toString();
        const QString preview = index.data(PreviewRole).toString();
        const QString time = index.data(TimeRole).toString();

        const QRect avatarRect(rowRect.left() + UiZoom::s(12), rowRect.top() + UiZoom::s(10),
                               UiZoom::s(40), UiZoom::s(40));
        if (!drawRemoteAvatar(painter, avatarRect, avatarUrl, UiZoom::s(8),
                              const_cast<QWidget*>(option.widget))) {
            const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
            painter->drawPixmap(avatarRect.topLeft(),
                                monogramAvatarPixmap(avatarMonogram(name, userId),
                                                     avatarRect.width(), UiZoom::s(8),
                                                     kBrandGradientFrom, kBrandGradientTo,
                                                     UiZoom::s(12), dpr));
        }

        const int textLeft = avatarRect.right() + UiZoom::s(14);
        // Size the time column to the actual text so short "HH:mm" stamps free up
        // room for the name and long dates never clip.
        QFont timeFont = option.font;
        timeFont.setPixelSize(UiZoom::s(12));
        const int timeWidth = time.isEmpty() ? 0 : QFontMetrics(timeFont).horizontalAdvance(time) + 2;
        const QRect timeRect(rowRect.right() - timeWidth - UiZoom::s(10), rowRect.top() + UiZoom::s(12), timeWidth, UiZoom::s(18));
        const int nameRight = time.isEmpty() ? rowRect.right() - UiZoom::s(12) : timeRect.left() - UiZoom::s(10);
        const QRect nameRect(textLeft, rowRect.top() + UiZoom::s(12), qMax(0, nameRight - textLeft), UiZoom::s(20));
        const QRect previewRect(textLeft, rowRect.top() + UiZoom::s(41), rowRect.right() - textLeft - UiZoom::s(12), UiZoom::s(18));

        QFont nameFont = option.font;
        nameFont.setPixelSize(UiZoom::s(14));
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
            badgeFont.setPixelSize(UiZoom::s(11));
            badgeFont.setBold(true);
            const int badgeHeight = UiZoom::s(18);
            const int badgeWidth = qMax(badgeHeight,
                                        QFontMetrics(badgeFont).horizontalAdvance(badgeText) + UiZoom::s(10));
            const QRect badgeRect(rowRect.right() - badgeWidth - UiZoom::s(10), previewRect.top(), badgeWidth, badgeHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#f53f3f")));
            painter->drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
            painter->setFont(badgeFont);
            painter->setPen(Qt::white);
            painter->drawText(badgeRect, Qt::AlignCenter, badgeText);
            clippedPreviewRect.setRight(badgeRect.left() - UiZoom::s(8));
        }

        QFont previewFont = option.font;
        previewFont.setPixelSize(UiZoom::s(13));
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
        return QSize(0, UiZoom::s(54));
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

        const QString userId = index.data(UserIdRole).toString();
        const QString name = index.data(DisplayNameRole).toString();
        const QString avatarUrl = index.data(AvatarUrlRole).toString();
        const QRect avatarRect(rowRect.left() + UiZoom::s(12), rowRect.top() + UiZoom::s(7),
                               UiZoom::s(36), UiZoom::s(36));
        if (!drawRemoteAvatar(painter, avatarRect, avatarUrl, UiZoom::s(8),
                              const_cast<QWidget*>(option.widget))) {
            const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
            painter->drawPixmap(avatarRect.topLeft(),
                                monogramAvatarPixmap(avatarMonogram(name, userId),
                                                     avatarRect.width(), UiZoom::s(8),
                                                     kBrandGradientFrom, kBrandGradientTo,
                                                     UiZoom::s(11), dpr));
        }

        const int textLeft = avatarRect.right() + UiZoom::s(14);
        const QRect nameRect(textLeft, rowRect.top(), rowRect.right() - textLeft - UiZoom::s(12), rowRect.height());

        QFont nameFont = option.font;
        nameFont.setPixelSize(UiZoom::s(14));
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
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClick_) {
            onClick_(imagePath_);
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    QString imagePath_;
    std::function<void(const QString&)> onClick_;
};

class ComposerTextEdit final : public QTextEdit {
public:
    explicit ComposerTextEdit(QWidget* parent = nullptr) : QTextEdit(parent) {
        setAcceptDrops(true);
    }

    // 把「拖进来/粘进来的东西」交给 MainWindow 决定是否变成内联附件。
    // 返回 true = 已消费，QTextEdit 不要再按默认方式插入。
    void setMimeHandler(std::function<bool(const QMimeData*)> handler) {
        mimeHandler_ = std::move(handler);
    }

    void setCornerAction(QWidget* action) {
        cornerAction_ = action;
        if (cornerAction_) {
            cornerAction_->setParent(this);
            positionCornerAction();
        }
    }

    void positionCornerAction() {
        if (!cornerAction_) return;
        const int inset = qMax(UiZoom::s(6), cornerAction_->width() / 5);
        cornerAction_->move(width() - cornerAction_->width() - inset,
                            height() - cornerAction_->height() - inset);
        cornerAction_->raise();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextEdit::resizeEvent(event);
        positionCornerAction();
    }

    // QTextEdit 的拖放与粘贴最终都汇到这两个钩子。不覆写的话，拖一个文件进来
    // 只会插入它的 file:/// URL 文本——用户看到的是一行路径，发出去的也是一行路径。
    bool canInsertFromMimeData(const QMimeData* source) const override {
        if (hasLocalFile(source)) return true;
        return QTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData* source) override {
        if (mimeHandler_ && mimeHandler_(source)) return;
        QTextEdit::insertFromMimeData(source);
    }

    // 拖到输入框上时给出「可以放」的反馈，否则 Windows 上是禁止光标。
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (hasLocalFile(event->mimeData())) {
            event->acceptProposedAction();
            return;
        }
        QTextEdit::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (hasLocalFile(event->mimeData())) {
            event->acceptProposedAction();
            return;
        }
        QTextEdit::dragMoveEvent(event);
    }

private:
    static bool hasLocalFile(const QMimeData* source) {
        if (!source || !source->hasUrls()) return false;
        for (const QUrl& url : source->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) return true;
        }
        return false;
    }

    QWidget* cornerAction_ = nullptr;
    std::function<bool(const QMimeData*)> mimeHandler_;
};

enum class LineIconKind {
    Messages = 1,
    Contacts,
    Settings,
    Search,
    Add,
    More,
    Copy,
    Preview,
    Download,
    Link,
    SelectAll,
    Trash,
    Screen,            // 远程桌面：显示器轮廓 + 底座
    ScreenConnecting,  // 连接中：显示器内三点
    ScreenDisconnect,  // 已连接，点击断开：显示器内叉
    Send,
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
        case LineIconKind::Copy:
        case LineIconKind::Preview:
        case LineIconKind::Download:
        case LineIconKind::Link:
        case LineIconKind::SelectAll:
        case LineIconKind::Trash:
        case LineIconKind::Screen:
        case LineIconKind::ScreenConnecting:
        case LineIconKind::ScreenDisconnect:
        case LineIconKind::Send:
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
        case LineIconKind::Screen:
            // 显示器：圆角屏幕 + 底座横杠，16px 下轮廓依然清晰。
            painter.drawRoundedRect(QRectF(9, 12, 30, 21), 3, 3);
            painter.drawLine(QPointF(24, 33), QPointF(24, 37));
            painter.drawLine(QPointF(17, 38), QPointF(31, 38));
            break;
        case LineIconKind::ScreenConnecting:
            // 同一台显示器 + 屏内三点：与就绪态形状一致，只有内部符号变化，
            // 避免图标整体跳动造成"换了个按钮"的错觉。
            painter.drawRoundedRect(QRectF(9, 12, 30, 21), 3, 3);
            painter.drawLine(QPointF(24, 33), QPointF(24, 37));
            painter.drawLine(QPointF(17, 38), QPointF(31, 38));
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(QRectF(16, 20, 5, 5));
            painter.drawEllipse(QRectF(21.5, 20, 5, 5));
            painter.drawEllipse(QRectF(27, 20, 5, 5));
            break;
        case LineIconKind::ScreenDisconnect:
            // 屏内打叉 = 点击断开。
            painter.drawRoundedRect(QRectF(9, 12, 30, 21), 3, 3);
            painter.drawLine(QPointF(24, 33), QPointF(24, 37));
            painter.drawLine(QPointF(17, 38), QPointF(31, 38));
            painter.drawLine(QPointF(18, 17), QPointF(30, 28));
            painter.drawLine(QPointF(30, 17), QPointF(18, 28));
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
        case LineIconKind::Copy:
            // 两张叠纸：先画后层，再用白底前层盖住重叠区（16px 下观感干净）。
            painter.drawRoundedRect(QRectF(16, 9, 22, 22), 5, 5);
            painter.setBrush(Qt::white);
            painter.drawRoundedRect(QRectF(10, 16, 22, 22), 5, 5);
            break;
        case LineIconKind::Preview: {
            // 眼睛：上下两条弧线 + 瞳孔。
            QPainterPath eye;
            eye.moveTo(8, 24);
            eye.quadTo(24, 8, 40, 24);
            eye.moveTo(8, 24);
            eye.quadTo(24, 40, 40, 24);
            painter.drawPath(eye);
            painter.drawEllipse(QRectF(19, 19, 10, 10));
            break;
        }
        case LineIconKind::Download:
            // 下载：箭头落进托盘。
            painter.drawLine(QPointF(24, 9), QPointF(24, 27));
            painter.drawLine(QPointF(16, 20), QPointF(24, 28));
            painter.drawLine(QPointF(32, 20), QPointF(24, 28));
            painter.drawPolyline(QPolygonF({QPointF(9, 30), QPointF(9, 37), QPointF(39, 37), QPointF(39, 30)}));
            break;
        case LineIconKind::Link:
            // 链接：斜向两节链环。
            painter.save();
            painter.translate(24, 24);
            painter.rotate(-45);
            painter.drawRoundedRect(QRectF(-17, -6, 19, 12), 6, 6);
            painter.drawRoundedRect(QRectF(-2, -6, 19, 12), 6, 6);
            painter.restore();
            break;
        case LineIconKind::SelectAll: {
            // 全选：虚线选择框。
            QPen dashPen(color, 4, Qt::CustomDashLine, Qt::RoundCap, Qt::RoundJoin);
            dashPen.setDashPattern({2.4, 2.4});
            painter.setPen(dashPen);
            painter.drawRoundedRect(QRectF(11, 11, 26, 26), 6, 6);
            break;
        }
        case LineIconKind::Trash:
            // 垃圾桶：提手 + 顶沿 + 桶身 + 两道内槽。
            painter.drawPolyline(QPolygonF({QPointF(18, 12), QPointF(18, 8), QPointF(30, 8), QPointF(30, 12)}));
            painter.drawLine(QPointF(9, 13), QPointF(39, 13));
            painter.drawRoundedRect(QRectF(14, 13, 20, 26), 3, 3);
            painter.drawLine(QPointF(21, 20), QPointF(21, 33));
            painter.drawLine(QPointF(27, 20), QPointF(27, 33));
            break;
        case LineIconKind::Send:
            // 纸飞机：常见的消息发送语义，缩小到按钮尺寸后仍保持清晰轮廓。
            painter.drawPolygon(QPolygonF({
                QPointF(8, 22),
                QPointF(40, 8),
                QPointF(30, 40),
                QPointF(23, 28),
            }));
            painter.drawLine(QPointF(8, 22), QPointF(23, 28));
            painter.drawLine(QPointF(23, 28), QPointF(40, 8));
            break;
    }
    painter.end();
    return QIcon(pixmap);
}

QPushButton* makeNavButton(const QString& title, const QString& objectName, QWidget* parent) {
    // 只留图标，不显示文字。文字改挂 tooltip 与 accessibleName：纯图标对
    // 「远程」「设置」这类不常点的入口本来就不好认，去掉文字后必须留个说明，
    // accessibleName 同时让读屏软件和测试仍能按名字找到它。
    auto* button = new QPushButton(parent);
    button->setObjectName(objectName);
    button->setToolTip(title);
    button->setAccessibleName(title);
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

// 附件右键菜单统一图标色（与聊天头部图标一致）。
const QColor kMenuIconColor(QStringLiteral("#4c5866"));

// 飞书式消息右键菜单：白底圆角卡片、条目悬浮淡蓝高亮、分组分隔线。
// QMenu 是原生弹窗，QSS 圆角需要无边框 + 透明底配合，否则圆角外露出直角底色。
void applyMessageContextMenuStyle(QMenu& menu) {
    menu.setWindowFlags(menu.windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    menu.setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QMenu {
            background: #ffffff;
            border: 1px solid #e2e8f0;
            border-radius: 10px;
            padding: 6px;
        }
        QMenu::item {
            background: transparent;
            color: #1f2329;
            font-size: 13px;
            padding: 8px 36px 8px 10px;
            border-radius: 6px;
        }
        QMenu::item:selected {
            background: #f2f6fb;
            color: #1f2329;
        }
        QMenu::icon {
            padding-left: 8px;
        }
        QMenu::separator {
            height: 1px;
            background: #eef2f6;
            margin: 5px 6px;
        }
    )")));
}

// 文本气泡右键菜单：复制（有选区复制选区，否则复制渲染后的整条消息）/
// 复制原始数据（保留 Markdown 源语法）/ 复制链接（点在链接上时出现）/ 全选。
// 样式与图片/文件气泡的菜单一致。
void MarkdownMessageView::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    applyMessageContextMenuStyle(menu);
    const QString anchor = anchorAt(event->pos());
    QAction* copyAction = menu.addAction(makeLineIcon(LineIconKind::Copy, kMenuIconColor),
                                         QStringLiteral("复制"));
    copyOriginalDataAction_->setIcon(makeLineIcon(LineIconKind::Copy, kMenuIconColor));
    menu.addAction(copyOriginalDataAction_);
    QAction* copyLinkAction = anchor.isEmpty()
        ? nullptr
        : menu.addAction(makeLineIcon(LineIconKind::Link, kMenuIconColor), QStringLiteral("复制链接"));
    menu.addSeparator();
    QAction* selectAllAction = menu.addAction(makeLineIcon(LineIconKind::SelectAll, kMenuIconColor),
                                              QStringLiteral("全选"));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == copyAction) {
        if (textCursor().hasSelection()) {
            copy();
        } else {
            QApplication::clipboard()->setText(document()->toPlainText());
        }
    } else if (copyLinkAction != nullptr && chosen == copyLinkAction) {
        QApplication::clipboard()->setText(anchor);
    } else if (chosen == selectAllAction) {
        selectAll();
    }
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

QString latestMessageText(const RemoteIMMessage* message) {
    if (!message) return QStringLiteral("暂无消息");
    QString text = message->text;
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

QString latestMessageTime(const RemoteIMMessage* message) {
    return message ? conversationListTimeText(message->createdAtMillis) : QString();
}

QString messageTimeText(const RemoteIMMessage& message) {
    return relativeMessageTimeText(message.createdAtMillis);
}

// 输入框里内联附件的资源名前缀：QTextEdit 只认得住一个「资源名」，把类型编码进去
// 是这里区分图片 / 文件 / 视频的唯一手段（图片没有前缀，资源名就是原路径）。
const QString kComposerFilePrefix = QStringLiteral("pending-file://");
const QString kComposerVideoPrefix = QStringLiteral("pending-video://");

bool isHtmlFile(const RemoteIMFileAttachment& attachment) {
    const QString mimeType = attachment.mimeType.toLower();
    const QString fileName = attachment.fileName.toLower();
    return mimeType.contains(QStringLiteral("html"))
        || fileName.endsWith(QStringLiteral(".html"))
        || fileName.endsWith(QStringLiteral(".htm"));
}

bool isMarkdownFile(const RemoteIMFileAttachment& attachment) {
    const QString mimeType = attachment.mimeType.toLower();
    const QString fileName = attachment.fileName.toLower();
    return mimeType.contains(QStringLiteral("markdown"))
        || fileName.endsWith(QStringLiteral(".md"))
        || fileName.endsWith(QStringLiteral(".markdown"));
}

// 视频走 IM 的视频消息发出去，但本地回显仍存成文件附件（本地库还没有视频列）。
// 气泡靠 MIME/扩展名认出来，显示成视频卡片而不是一张「文档」卡。
bool isVideoFile(const RemoteIMFileAttachment& attachment) {
    const QString mimeType = attachment.mimeType.toLower();
    const QString fileName = attachment.fileName.toLower();
    return mimeType.startsWith(QStringLiteral("video/"))
        || fileName.endsWith(QStringLiteral(".mp4"))
        || fileName.endsWith(QStringLiteral(".mov"));
}

// 仅 md/html 文档支持内嵌预览；其余是普通文件，点击/菜单走「另存为」。
bool isPreviewableDocument(const RemoteIMFileAttachment& attachment) {
    return isHtmlFile(attachment) || isMarkdownFile(attachment);
}

QString fileSizeText(qint64 bytes) {
    if (bytes <= 0) return QString();
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
    if (bytes < qint64(1024) * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
    }
    return QStringLiteral("%1 GB").arg(QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
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
    // 缩放只作用于主界面：登录窗（先于此构造）保持设计尺寸，
    // 进入主界面时才把全局字体切到基准 13px × 倍率。
    QFont scaledFont = QApplication::font();
    scaledFont.setPixelSize(UiZoom::s(13));
    QApplication::setFont(scaledFont);
    buildUi();
    applyStyle();
    bindSignals();
    // 必须在 buildUi 之后：设置页的控件已建好，这里创建控制器并把当前配置
    // 回填到界面上。
    setupRemoteDesktop();
    refreshRemoteDesktopSettings();
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                // macOS 从“辅助功能”设置切回来后立即刷新授权状态。
                if (state == Qt::ApplicationActive) refreshRemoteDesktopSettings();
            });
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            [this] { stopRemoteDesktopForShutdown(); });
    refresh();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (remoteDesktopShutdownComplete_) {
        QMainWindow::closeEvent(event);
        return;
    }

    // IM 发送是异步的。先拦住本次关闭，等 stop 已交给 SDK（或短超时）
    // 再真正关闭，否则进程退出会让对端永远停在共享/控制状态。
    event->ignore();
    if (remoteDesktopShutdown_) return;

    QPointer<MainWindow> window(this);
    stopRemoteDesktopForShutdown([window] {
        if (!window) return;
        QTimer::singleShot(0, window, [window] {
            if (window) window->close();
        });
    });
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
        // 结果面板是按搜索框位置摆的浮层，不在布局里，窗口一变它就得重新对位。
        layoutGlobalSearchResults();
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
    resize(UiZoom::s(1280), UiZoom::s(820));
    setMinimumSize(UiZoom::s(980), UiZoom::s(640));

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("root"));
    // 纵向根布局：共享指示条常驻最顶部并横贯整宽，下方才是原有的横向主体。
    // 指示条必须压在所有内容之上，不能被侧栏或会话区挤掉。
    auto* rootColumn = new QVBoxLayout(root);
    rootColumn->setContentsMargins(0, 0, 0, 0);
    rootColumn->setSpacing(0);
    setCentralWidget(root);

    sharingIndicator_ = new SharingIndicatorBar(root);
    rootColumn->addWidget(sharingIndicator_);

    // 结果面板浮在窗口上，不进布局：它要盖住下方内容，进布局会把主体挤下去。
    globalSearchResults_ = new QListWidget(root);
    globalSearchResults_->setObjectName(QStringLiteral("globalSearchResults"));
    globalSearchResults_->setWindowFlags(Qt::Widget);
    globalSearchResults_->setFrameShape(QFrame::NoFrame);
    globalSearchResults_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    globalSearchResults_->hide();

    auto* rootContent = new QWidget(root);
    rootContent->setObjectName(QStringLiteral("rootContent"));
    auto* rootLayout = new QHBoxLayout(rootContent);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootColumn->addWidget(rootContent, 1);

    auto* rootNavigationSplitter = new QSplitter(Qt::Horizontal, rootContent);
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
    // 纯图标之后不需要那么宽：按「图标 + 两侧留白」定宽，不再让它可拉伸。
    navRail_->setFixedWidth(UiZoom::s(64));
    auto* navLayout = new QVBoxLayout(navRail_);
    navLayout->setContentsMargins(8, 14, 8, 12);
    navLayout->setSpacing(8);

    auto* logo = new QLabel(navRail_);
    logo->setObjectName(QStringLiteral("navLogo"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setPixmap(monogramAvatarPixmap(QStringLiteral("M"), UiZoom::s(34), UiZoom::s(17),
                                         kBrandGradientFrom, kBrandGradientTo,
                                         UiZoom::s(15), logo->devicePixelRatioF()));
    // 头像独占一行居中；「添加联系人」已移到会话栏的搜索框旁边（微信式）。
    navLayout->addWidget(logo, 0, Qt::AlignHCenter);
    navLayout->addSpacing(UiZoom::s(6));

    messageNavButton_ = makeNavButton(QStringLiteral("消息"), QStringLiteral("messagesNavButton"), navRail_);
    contactsNavButton_ = makeNavButton(QStringLiteral("通讯录"), QStringLiteral("contactsNavButton"), navRail_);
    // 远程桌面画面在应用内成页展示，不再弹独立窗口。
    remoteNavButton_ = makeNavButton(QStringLiteral("远程"), QStringLiteral("remoteNavButton"), navRail_);
    settingsNavButton_ = makeNavButton(QStringLiteral("设置"), QStringLiteral("settingsNavButton"), navRail_);
    messageNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Messages));
    contactsNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Contacts));
    remoteNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Screen));
    settingsNavButton_->setProperty("navIconKind", lineIconKindValue(LineIconKind::Settings));
    messageNavButton_->setProperty("selected", true);
    for (QPushButton* navButton :
         {messageNavButton_, contactsNavButton_, remoteNavButton_, settingsNavButton_}) {
        navButton->setIconSize(QSize(kNavIconPixels, kNavIconPixels));
    }
    applyNavButtonIcon(messageNavButton_, true);
    applyNavButtonIcon(contactsNavButton_, false);
    applyNavButtonIcon(remoteNavButton_, false);
    applyNavButtonIcon(settingsNavButton_, false);
    navLayout->addWidget(messageNavButton_);
    navLayout->addWidget(contactsNavButton_);
    navLayout->addWidget(remoteNavButton_);
    navLayout->addWidget(settingsNavButton_);
    navLayout->addStretch(1);

    auto* conversationPane = new QWidget(messagesPage_);
    conversationPane->setObjectName(QStringLiteral("conversationPane"));
    conversationPane->setMinimumWidth(UiZoom::s(220));
    auto* conversationLayout = new QVBoxLayout(conversationPane);
    conversationLayout->setContentsMargins(20, 18, 16, 16);
    conversationLayout->setSpacing(14);

    // 会话栏头部：搜索框 + 添加联系人，与微信同一形态。
    // 搜索原先横贯窗口顶部，但它搜的结果最终都落在这一列里，放在这列的头部
    // 更符合「在哪找、结果在哪」的直觉，也省掉一整条顶栏的高度。
    auto* conversationHeader = new QHBoxLayout();
    conversationHeader->setContentsMargins(0, 0, 0, 0);
    conversationHeader->setSpacing(8);
    navSearchInput_ = new QLineEdit(conversationPane);
    navSearchInput_->setObjectName(QStringLiteral("globalSearchBox"));
    navSearchInput_->setPlaceholderText(QStringLiteral("搜索消息 (Ctrl+F)"));
    navSearchInput_->setClearButtonEnabled(true);
    navSearchInput_->addAction(makeLineIcon(LineIconKind::Search, QColor(QStringLiteral("#98a2b3"))),
                               QLineEdit::LeadingPosition);
    addContactButton_ = new QPushButton(conversationPane);
    addContactButton_->setObjectName(QStringLiteral("addConversationButton"));
    addContactButton_->setIcon(makeLineIcon(LineIconKind::Add, QColor(QStringLiteral("#4c5866"))));
    addContactButton_->setIconSize(QSize(kNavIconPixels, kNavIconPixels));
    addContactButton_->setToolTip(QStringLiteral("添加联系人"));
    addContactButton_->setCursor(Qt::PointingHandCursor);
    conversationHeader->addWidget(navSearchInput_, 1);
    conversationHeader->addWidget(addContactButton_, 0);

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
    chatContentPane->setMinimumWidth(UiZoom::s(520));
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
    // 远程桌面入口：跟着当前会话走——正在跟谁聊天就远程谁，无需另选设备。
    remoteDesktopButton_ =
        makeHeaderIconButton(LineIconKind::Screen, QStringLiteral("远程桌面"), header);
    remoteDesktopButton_->setObjectName(QStringLiteral("remoteDesktopButton"));
    connect(remoteDesktopButton_, &QPushButton::clicked, this,
            &MainWindow::requestRemoteDesktop);
    headerLayout->addWidget(remoteDesktopButton_);
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
    composer->setMinimumHeight(UiZoom::s(116));
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

    messageEditor_ = new ComposerTextEdit(composer);
    messageEditor_->setObjectName(QStringLiteral("messageEditor"));
    messageEditor_->setPlaceholderText(QStringLiteral("输入消息（可拖入文件，或 Ctrl+V 粘贴图片/文件）"));
    messageEditor_->setAcceptRichText(false);
    messageEditor_->setMinimumHeight(UiZoom::s(64));
    messageEditor_->installEventFilter(this);
    // 拖进来的文件走和 Ctrl+V 完全相同的路由，不再被当成 file:/// 文本插入。
    static_cast<ComposerTextEdit*>(messageEditor_)->setMimeHandler(
        [this](const QMimeData* mime) { return insertComposerMimeData(mime); });

    sendButton_ = new QPushButton(messageEditor_);
    sendButton_->setObjectName(QStringLiteral("sendButton"));
    sendButton_->setIcon(makeLineIcon(LineIconKind::Send, QColor(QStringLiteral("#ffffff"))));
    sendButton_->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    sendButton_->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    sendButton_->setToolTip(QStringLiteral("发送消息"));
    sendButton_->setAccessibleName(QStringLiteral("发送消息"));
    sendButton_->setCursor(Qt::PointingHandCursor);
    static_cast<ComposerTextEdit*>(messageEditor_)->setCornerAction(sendButton_);

    composerLayout->addWidget(messageEditor_, 1);

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
    contactsDirectoryPane->setMinimumWidth(UiZoom::s(300));
    contactsDirectoryPane->setMaximumWidth(UiZoom::s(420));
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
    // 设置项只会越加越多。不套滚动区的话，内容一超过页面高度 Qt 就按比例
    // 压扁各行——「被控模式」那三个单选会叠在一起糊成一团（加「允许远程控制」
    // 那行时就压过线了）。
    auto* settingsPageLayout = new QVBoxLayout(settingsPage_);
    settingsPageLayout->setContentsMargins(0, 0, 0, 0);
    settingsPageLayout->setSpacing(0);
    auto* settingsScroll = new QScrollArea(settingsPage_);
    settingsScroll->setObjectName(QStringLiteral("settingsScroll"));
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsPageLayout->addWidget(settingsScroll);

    auto* settingsContent = new QWidget(settingsScroll);
    settingsContent->setObjectName(QStringLiteral("settingsContent"));
    settingsScroll->setWidget(settingsContent);
    auto* settingsLayout = new QVBoxLayout(settingsContent);
    settingsLayout->setContentsMargins(36, 30, 36, 30);
    settingsLayout->setSpacing(18);

    auto* settingsTitle = new QLabel(QStringLiteral("设置"), settingsContent);
    settingsTitle->setObjectName(QStringLiteral("pageTitle"));
    auto* settingsSubtitle = new QLabel(QStringLiteral("桌面端 IM 使用和移动端一致的内置连接配置。"), settingsContent);
    settingsSubtitle->setObjectName(QStringLiteral("settingsSubtitle"));

    auto* settingsPanel = new QWidget(settingsContent);
    settingsPanel->setObjectName(QStringLiteral("settingsPanel"));
    auto* settingsPanelLayout = new QVBoxLayout(settingsPanel);
    // 上下留白：边距为 0 时首行标题会紧贴面板上边框，末行也贴着下边框。
    // 左右仍为 0，让行的分隔线横贯整个面板宽度。
    settingsPanelLayout->setContentsMargins(0, UiZoom::s(8), 0, UiZoom::s(8));
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
    settingsLayout->addSpacing(UiZoom::s(24));
    settingsLayout->addWidget(buildRemoteDesktopSettingsPanel(settingsContent));
    settingsLayout->addStretch(1);

    // 远程桌面页：画面在应用内展示，不弹独立窗口。
    remotePage_ = new QWidget(contentStack_);
    remotePage_->setObjectName(QStringLiteral("remotePage"));
    auto* remoteLayout = new QVBoxLayout(remotePage_);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(0);
    remoteDesktopView_ = new RemoteDesktopViewPanel(remotePage_);
    remoteLayout->addWidget(remoteDesktopView_);
    connect(remoteDesktopView_, &RemoteDesktopViewPanel::fullScreenChanged, this,
            &MainWindow::applyRemoteDesktopFullScreen);
    connect(remoteDesktopView_, &RemoteDesktopViewPanel::controlToggleRequested, this,
            &MainWindow::toggleRemoteDesktopControl);

    contentStack_->addWidget(messagesPage_);
    contentStack_->addWidget(contactsPage_);
    contentStack_->addWidget(remotePage_);
    contentStack_->addWidget(settingsPage_);

    rootNavigationSplitter->addWidget(navRail_);
    rootNavigationSplitter->addWidget(contentStack_);
    rootNavigationSplitter->setStretchFactor(0, 0);
    rootNavigationSplitter->setStretchFactor(1, 1);
    rootNavigationSplitter->setSizes(QList<int>() << 180 << 1100);
    rootLayout->addWidget(rootNavigationSplitter, 1);
}

void MainWindow::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
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
            background: transparent;
        }
        #addConversationButton {
            min-width: 30px;
            max-width: 30px;
            min-height: 30px;
            border: 1px solid #dbe4ef;
            border-radius: 8px;
            background: #ffffff;
            padding: 0;
        }
        #addConversationButton:hover {
            border-color: #8ed0ff;
            background: #f2f9ff;
        }
        #globalSearchBox {
            min-height: 34px;
            border: 1px solid #dbe6f3;
            border-radius: 9px;
            background: #ffffff;
            color: #172033;
            padding: 0 8px;
            font-size: 13px;
        }
        #globalSearchBox:focus {
            border-color: #8ed0ff;
        }
        #globalSearchResults {
            background: #ffffff;
            border: 1px solid #dbe4ef;
            border-radius: 10px;
            padding: 4px;
            font-size: 13px;
        }
        #globalSearchResults::item {
            border-radius: 8px;
            padding: 6px 10px;
            color: #33415a;
        }
        #globalSearchResults::item:selected, #globalSearchResults::item:hover {
            background: #e6eef8;
            color: #0b67b7;
        }
        /* 纯图标：不再留文字的左内边距，图标居中，宽高一致，
           否则一列图标会因为各自的留白不同而看起来大小不一。 */
        #messagesNavButton, #contactsNavButton, #remoteNavButton, #settingsNavButton {
            min-width: 40px;
            max-width: 40px;
            min-height: 40px;
            max-height: 40px;
            border: 0;
            border-radius: 10px;
            background: transparent;
            padding: 0;
        }
        #messagesNavButton[selected="true"], #contactsNavButton[selected="true"], #remoteNavButton[selected="true"], #settingsNavButton[selected="true"] {
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
        #contactsPage, #settingsPage, #settingsContent, #settingsScroll {
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
        /* 左边距跟各行标题（行内边距 18px）对齐；上边距不能是 0，
           否则标题直接贴在面板上边框上，看着像挤在线上。 */
        #settingsSectionTitle {
            color: #0f172a;
            font-size: 15px;
            font-weight: 800;
            padding: 6px 0 12px 18px;
        }
        #settingsRadio {
            color: #334155;
            font-size: 13px;
            spacing: 8px;
        }
        #settingsRowButton {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            border-radius: 8px;
            color: #1e40af;
            font-size: 13px;
            font-weight: 700;
            padding: 6px 16px;
        }
        #settingsRowButton:hover {
            background: #e2e8f0;
        }
        /* 背景必须透明：面板布局边距为 0，行控件正好铺在面板那 1px 边框上，
           行一旦有不透明背景就会把左右竖边整段盖掉（只剩标题区那一小截还在）。
           白底由 #settingsPanel 提供，行只负责底部分隔线。 */
        #settingsRow {
            background: transparent;
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
            padding: 10px 52px 46px 13px;
            font-size: 14px;
        }
        #messageEditor:focus {
            border-color: #58b7ff;
        }
        #sendButton {
            border-radius: 8px;
            border: 0;
            background: #168eea;
            padding: 0;
        }
        #sendButton:hover {
            background: #087ed2;
        }
        #sendButton:pressed {
            background: #066db7;
        }
        #sendButton:disabled {
            background: #c4def0;
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
    )")));
}

void MainWindow::bindSignals() {
    // 整体缩放（飞书式）：Ctrl+= / Ctrl++（小键盘）放大，Ctrl+- 缩小，Ctrl+0 复位。
    connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Equal), this), &QShortcut::activated,
            this, [this] { changeUiZoom(UiZoom::step()); });
    connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Plus), this), &QShortcut::activated,
            this, [this] { changeUiZoom(UiZoom::step()); });
    connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Minus), this), &QShortcut::activated,
            this, [this] { changeUiZoom(-UiZoom::step()); });
    connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_0), this), &QShortcut::activated,
            this, [this] { resetUiZoom(); });

    connect(&app_, &RemoteIMApplication::stateChanged, this, [this] { refresh(); });
    connect(&app_, &RemoteIMApplication::selectionChanged, this, [this](const QString&) {
        refreshSelectedConversation();
    });
    connect(&app_, &RemoteIMApplication::connectionChanged, this, [this](bool connected) {
        statusLabel_->setText(connected ? QStringLiteral("● 已连接") : QStringLiteral("● 未连接"));
        refreshSettings();
    });
    connect(&app_, &RemoteIMApplication::errorMessage, this, [this](const QString& message) {
        if (QCoreApplication::arguments().contains(QStringLiteral("--smoke"))) return;
        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning, QStringLiteral("IM"), message);
    });
    connect(addContactButton_, &QPushButton::clicked, this, [this] { openAddContactDialog(); });
    // 搜索一次要扫所有会话的消息，绝不能挂在每次按键上跑：会话上千条时输入会发涩。
    // 与命令提示条同一套做法——停顿 150ms 才真正执行，输入法组词期间一律不跑。
    globalSearchUpdateTimer_ = new QTimer(this);
    globalSearchUpdateTimer_->setSingleShot(true);
    globalSearchUpdateTimer_->setInterval(150);
    connect(globalSearchUpdateTimer_, &QTimer::timeout, this, [this] {
        // 必须先搜索再过滤：过滤要用搜索算出的 peersWithSearchHits_，
        // 反过来的话会话列表用的是上一次的结果，看着像「少了一个会话」。
        refreshGlobalSearchResults();
        applyConversationFilter();
    });
    connect(navSearchInput_, &QLineEdit::textChanged, this, [this] {
        // 清空是个例外：立刻收起结果、恢复会话列表，不该让人等 150ms。
        if (navSearchInput_->text().trimmed().isEmpty()) {
            globalSearchUpdateTimer_->stop();
            refreshGlobalSearchResults();
            applyConversationFilter();
            return;
        }
        globalSearchUpdateTimer_->start();
    });
    connect(globalSearchResults_, &QListWidget::itemClicked, this,
            &MainWindow::openGlobalSearchResult);
    connect(globalSearchResults_, &QListWidget::itemActivated, this,
            &MainWindow::openGlobalSearchResult);
    // 回车直接打开第一条命中，不必先用方向键选中。
    connect(navSearchInput_, &QLineEdit::returnPressed, this, [this] {
        if (!globalSearchResults_ || globalSearchResults_->count() == 0) return;
        QListWidgetItem* first = globalSearchResults_->currentItem();
        if (!first) first = globalSearchResults_->item(0);
        openGlobalSearchResult(first);
    });
    auto* globalSearchShortcut = new QShortcut(QKeySequence::Find, this);
    connect(globalSearchShortcut, &QShortcut::activated, this, &MainWindow::focusGlobalSearch);
    auto* globalSearchEscape = new QShortcut(QKeySequence(Qt::Key_Escape), navSearchInput_);
    globalSearchEscape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(globalSearchEscape, &QShortcut::activated, this, &MainWindow::closeGlobalSearchResults);
    connect(messageNavButton_, &QPushButton::clicked, this, [this] { showMessagesPage(); });
    connect(contactsNavButton_, &QPushButton::clicked, this, [this] { showContactsPage(); });
    connect(remoteNavButton_, &QPushButton::clicked, this, [this] { showRemotePage(); });
    connect(settingsNavButton_, &QPushButton::clicked, this, [this] { showSettingsPage(); });
    connect(contentStack_, &QStackedWidget::currentChanged, this, [this] { syncNavigationSelection(); });
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
        showConversationContextMenu(pos);
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
    updateRemoteDesktopButton();
}

void MainWindow::refreshSelectedConversation() {
    const QString selectedPeer = app_.chatState().selectedPeerId();
    {
        QSignalBlocker blocker(conversationList_);
        for (int row = 0; row < conversationList_->count(); ++row) {
            QListWidgetItem* item = conversationList_->item(row);
            if (item->data(UserIdRole).toString() != selectedPeer) continue;
            if (conversationList_->currentItem() != item) conversationList_->setCurrentItem(item);
            if (item->data(UnreadRole).toInt() != 0) item->setData(UnreadRole, 0);
            break;
        }
    }
    conversationList_->viewport()->update();
    refreshMessages();
    updateRemoteDesktopButton();
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
        item->setSizeHint(QSize(0, UiZoom::s(76)));
        RemoteIMMessage latestMessage;
        const bool hasLatestMessage = app_.chatState().latestMessageWith(contact.userId, &latestMessage);
        item->setData(UserIdRole, contact.userId);
        item->setData(DisplayNameRole, contact.displayName.isEmpty() ? contact.userId : contact.displayName);
        item->setData(PreviewRole, latestMessageText(hasLatestMessage ? &latestMessage : nullptr));
        item->setData(TimeRole, latestMessageTime(hasLatestMessage ? &latestMessage : nullptr));
        item->setData(UnreadRole, app_.chatState().unreadCount(contact.userId));
        item->setData(AvatarUrlRole, contact.avatarUrl);
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
        // 会话里有命中的消息也要留下。否则搜一个只出现在聊天记录深处的词，
        // 左边整列会空掉——而右边的结果面板明明列着这些会话，自相矛盾。
        // 「会话里有没有命中」由上一趟搜索算好放在 peersWithSearchHits_ 里，
        // 这里直接查，不再把所有会话重新扫一遍——否则一次输入要扫两遍。
        const bool matched = needle.isEmpty()
            || name.contains(needle, Qt::CaseInsensitive)
            || preview.contains(needle, Qt::CaseInsensitive)
            || peersWithSearchHits_.contains(item->data(UserIdRole).toString());
        item->setHidden(!matched);
    }
}

void MainWindow::focusGlobalSearch() {
    if (!navSearchInput_) return;
    navSearchInput_->setFocus();
    navSearchInput_->selectAll();
    refreshGlobalSearchResults();
}

void MainWindow::closeGlobalSearchResults() {
    if (globalSearchResults_) globalSearchResults_->hide();
}

void MainWindow::layoutGlobalSearchResults() {
    if (!globalSearchResults_ || !navSearchInput_ || !globalSearchResults_->isVisible()) return;
    // 面板挂在窗口上而不是搜索框里：它要盖住下方内容，塞进布局会把主体挤下去。
    const QPoint topLeft =
        navSearchInput_->mapTo(this, QPoint(0, navSearchInput_->height() + UiZoom::s(6)));
    const int rows = qMin(globalSearchResults_->count(), 8);
    const int height = qMax(UiZoom::s(56), rows * UiZoom::s(52) + UiZoom::s(8));
    globalSearchResults_->setGeometry(topLeft.x(), topLeft.y(), navSearchInput_->width(), height);
    globalSearchResults_->raise();
}

void MainWindow::refreshGlobalSearchResults() {
    if (!navSearchInput_ || !globalSearchResults_) return;
    const QString needle = navSearchInput_->text().trimmed();
    globalSearchResults_->clear();
    peersWithSearchHits_.clear();
    if (needle.isEmpty()) {
        closeGlobalSearchResults();
        return;
    }

    struct Hit {
        QString peerId;
        QString peerName;
        QString messageId;
        QString preview;
        qint64 createdAt = 0;
        int score = 0;
    };

    const ChatState& state = app_.chatState();
    QVector<Hit> hits;
    for (const RemoteIMContact& contact : state.contacts()) {
        const QString peerName = contact.displayName.isEmpty() ? contact.userId : contact.displayName;
        // 逐条流式判断，不调 messagesWith——那会把整个会话深拷贝一份出来，
        // 放在搜索里等于每次输入都复制上千条消息。
        state.forEachMessageWith(contact.userId, [&](const RemoteIMMessage& message) {
            const int score = MessageSearch::score(message.text, needle);
            if (score == MessageSearch::NoMatch) return;
            // 同一趟顺便记下「这个会话有命中」，会话列表过滤直接复用，不再扫第二遍。
            peersWithSearchHits_.insert(contact.userId);
            hits.append({contact.userId, peerName, message.id, message.text.simplified(),
                         message.createdAtMillis, score});
        });
    }

    // 先按贴切度，再按时间新→旧。模糊命中排在原样命中后面，
    // 这样「记岔一点也能搜到」不会把真正想找的那条挤下去。
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.createdAt > b.createdAt;
    });

    const bool truncated = hits.size() > MaxGlobalSearchResults;
    const int shown = truncated ? MaxGlobalSearchResults : hits.size();
    for (int i = 0; i < shown; ++i) {
        const Hit& hit = hits.at(i);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 · %2\n%3")
                .arg(hit.peerName,
                     QDateTime::fromMSecsSinceEpoch(hit.createdAt)
                         .toString(QStringLiteral("MM-dd HH:mm")),
                     hit.preview));
        item->setData(SearchPeerRole, hit.peerId);
        item->setData(SearchMessageRole, hit.messageId);
        item->setSizeHint(QSize(0, UiZoom::s(52)));
        globalSearchResults_->addItem(item);
    }

    if (hits.isEmpty()) {
        // 说清范围：没点过「加载更早」的历史不在内存里，也就搜不到。
        auto* empty = new QListWidgetItem(QStringLiteral("无结果\n只搜索各会话中已加载的消息"));
        empty->setFlags(Qt::NoItemFlags);
        empty->setSizeHint(QSize(0, UiZoom::s(52)));
        globalSearchResults_->addItem(empty);
    } else if (truncated) {
        auto* more = new QListWidgetItem(
            QStringLiteral("结果过多，仅显示最贴切的 %1 条\n再输入几个字缩小范围")
                .arg(MaxGlobalSearchResults));
        more->setFlags(Qt::NoItemFlags);
        more->setSizeHint(QSize(0, UiZoom::s(52)));
        globalSearchResults_->addItem(more);
    }

    globalSearchResults_->show();
    layoutGlobalSearchResults();
}

void MainWindow::openGlobalSearchResult(QListWidgetItem* item) {
    if (!item) return;
    const QString peerId = item->data(SearchPeerRole).toString();
    const QString messageId = item->data(SearchMessageRole).toString();
    if (peerId.isEmpty() || messageId.isEmpty()) return;

    clearMessageSearchHighlight();
    if (peerId != app_.chatState().selectedPeerId()) {
        if (composerHasAttachments()) messageEditor_->clear();
        app_.selectPeer(peerId);
    }
    showMessagesPage();
    closeGlobalSearchResults();
    // 切会话会整屏重建气泡，气泡高度还要等一次布局才定下来，
    // 所以定位放到事件循环下一轮，否则滚到的位置是旧布局算出来的。
    QTimer::singleShot(0, this, [this, messageId] { highlightMessage(messageId); });
}

void MainWindow::highlightMessage(const QString& messageId) {
    clearMessageSearchHighlight();
    QWidget* row = messageRowById_.value(messageId);
    if (!row) return;
    messageSearchHighlightedId_ = messageId;
    row->setProperty("searchHit", true);
    row->style()->unpolish(row);
    row->style()->polish(row);
    if (messageScroll_) messageScroll_->ensureWidgetVisible(row, 0, UiZoom::s(60));
}

void MainWindow::clearMessageSearchHighlight() {
    if (messageSearchHighlightedId_.isEmpty()) return;
    if (QWidget* row = messageRowById_.value(messageSearchHighlightedId_)) {
        row->setProperty("searchHit", false);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    messageSearchHighlightedId_.clear();
}

void MainWindow::refreshContactDirectory() {
    contactsList_->blockSignals(true);
    contactsList_->clear();
    const QList<RemoteIMContact> contacts = app_.chatState().contacts();
    for (const RemoteIMContact& contact : contacts) {
        auto* item = new QListWidgetItem();
        item->setSizeHint(QSize(0, UiZoom::s(54)));
        item->setData(UserIdRole, contact.userId);
        item->setData(DisplayNameRole, contact.displayName.isEmpty() ? contact.userId : contact.displayName);
        item->setData(AvatarUrlRole, contact.avatarUrl);
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

void MainWindow::showRemotePage() {
    contentStack_->setCurrentWidget(remotePage_);
    syncNavigationSelection();
}

void MainWindow::syncNavigationSelection() {
    if (contentStack_->currentWidget() == contactsPage_) {
        updateNavigationSelection(contactsNavButton_);
        return;
    }
    if (contentStack_->currentWidget() == remotePage_) {
        updateNavigationSelection(remoteNavButton_);
        return;
    }
    if (contentStack_->currentWidget() == settingsPage_) {
        updateNavigationSelection(settingsNavButton_);
        return;
    }
    updateNavigationSelection(messageNavButton_);
}

void MainWindow::updateNavigationSelection(QPushButton* selectedButton) {
    const QList<QPushButton*> buttons = {messageNavButton_, contactsNavButton_, remoteNavButton_,
                                         settingsNavButton_};
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
    // Friend/profile callbacks can arrive after history messages. Rebuild only when
    // a rendered sender's display name or avatar URL changed, so existing bubbles
    // adopt the real profile image without turning routine message updates into a
    // full-list refresh.
    if (!needFullRebuild) {
        const QList<QLabel*> avatars = messageContainer_->findChildren<QLabel*>();
        for (const QLabel* avatar : avatars) {
            const QString userId = avatar->property("avatarUserId").toString();
            if (userId.isEmpty()) continue;
            QString avatarUrl;
            for (const RemoteIMContact& contact : app_.chatState().contacts()) {
                if (contact.userId == userId) {
                    avatarUrl = contact.avatarUrl.trimmed();
                    break;
                }
            }
            if (avatar->property("avatarDisplayName").toString() != contactName(userId)
                    || avatar->property("avatarUrl").toString() != avatarUrl) {
                needFullRebuild = true;
                break;
            }
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
        emptyView->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
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
        )")));
        messageLayout_->addWidget(emptyView);
        return;
    }

    // 布局固定结构：[0]=加载更早按钮（无更早时隐藏），随后消息行，末尾弹簧。
    loadEarlierButton_ = new QPushButton(QStringLiteral("加载更早的消息"), messageContainer_);
    loadEarlierButton_->setObjectName(QStringLiteral("loadEarlierButton"));
    loadEarlierButton_->setCursor(Qt::PointingHandCursor);
    loadEarlierButton_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
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
    )")));
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
    return insertComposerMimeData(QApplication::clipboard()->mimeData());
}

bool MainWindow::insertComposerMimeData(const QMimeData* mime) {
    if (app_.chatState().selectedPeerId().isEmpty()) return false;
    if (!mime) return false;

    // 1) 本地文件（资源管理器复制或直接拖进来的）：内联插入到输入框。
    if (mime->hasUrls()) {
        QStringList files;
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            if (QFileInfo(path).isFile()) files << path;
        }
        if (!files.isEmpty()) {
            for (const QString& path : files) {
                // mp4/mov 走 IM 的视频消息（对端能直接播）；能被 Qt 认出的图片按图片发
                // （对端气泡里直接出图、可预览）；其余一律按文件卡发。图片判断走内容而非
                // 扩展名，HEIC 之类 Qt 读不了的会自然落到文件卡，不会变成一张打不开的破图。
                if (isSupportedVideoFile(path)) {
                    insertComposerVideo(path);
                } else if (!QImageReader::imageFormat(path).isEmpty()) {
                    insertComposerImageFile(path);
                } else {
                    insertComposerFile(path);
                }
            }
            return true;
        }
    }

    // 2) 图像数据（截图工具、复制的图片，没有对应磁盘文件）：内联插入到输入框。
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            insertComposerImage(image);
            return true;
        }
    }
    return false;  // 交给 QTextEdit 默认处理（插入文本）
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

void MainWindow::insertComposerImageFile(const QString& localPath) {
    // 资源名直接用原文件路径：QTextEdit 从磁盘加载渲染，collectComposerAttachments
    // 也据此判定为图片（没有 pending-file:// 前缀），发送时发的就是这个原文件——
    // 不像剪贴板图像那样先落一份 PNG，3MB 的 JPG 不会被重编码成十几 MB 的 PNG。
    QImageReader reader(localPath);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty()) {
        insertComposerFile(localPath);  // 读不出尺寸就别硬塞，退回文件卡
        return;
    }

    QTextImageFormat fmt;
    fmt.setName(localPath);
    int w = size.width();
    int h = size.height();
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
    insertComposerChip(localPath, QStringLiteral("📄"), kComposerFilePrefix);
}

void MainWindow::insertComposerVideo(const QString& localPath) {
    // 视频同理，只是前缀换成 pending-video://，发送时走 IM 的视频消息而不是文件消息。
    insertComposerChip(localPath, QStringLiteral("🎬"), kComposerVideoPrefix);
}

void MainWindow::insertComposerChip(const QString& localPath, const QString& icon, const QString& resourcePrefix) {
    QString shown = QFileInfo(localPath).fileName();
    if (shown.size() > 22) shown = shown.left(19) + QStringLiteral("…");
    const QString label = icon + QStringLiteral(" ") + shown;

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

    const QString resourceName = resourcePrefix + localPath;
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
    const QTextDocument* doc = messageEditor_->document();
    // 按文档顺序取出所有内联对象（图片/文件）。
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isImageFormat()) continue;
            const QString name = frag.charFormat().toImageFormat().name();
            if (name.startsWith(kComposerFilePrefix)) {
                attachments.append(ComposerAttachment{ComposerAttachment::Kind::File,
                                                      name.mid(kComposerFilePrefix.size())});
            } else if (name.startsWith(kComposerVideoPrefix)) {
                attachments.append(ComposerAttachment{ComposerAttachment::Kind::Video,
                                                      name.mid(kComposerVideoPrefix.size())});
            } else {
                attachments.append(ComposerAttachment{ComposerAttachment::Kind::Image, name});
            }
        }
    }
    return attachments;
}

void MainWindow::openVideoPreview(const RemoteIMVideoAttachment& attachment) {
    const QString path = attachment.localPath.trimmed();
    if (path.isEmpty() || !QFile::exists(path)) {
        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                               QStringLiteral("无法播放"),
                               QStringLiteral("视频尚未下载完成或本地缓存已被清理。"));
        return;
    }
    const QString title = attachment.fileName.trimmed().isEmpty()
        ? QFileInfo(path).fileName()
        : attachment.fileName.trimmed();
    auto* dialog = new VideoPreviewDialog(path, title, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openImagePreview(const QString& imagePath) {
    if (imagePreviewDialog_) {
        imagePreviewDialog_->raise();
        imagePreviewDialog_->activateWindow();
        return;
    }

    auto* dialog = new ImagePreviewDialog(imagePath, this);
    imagePreviewDialog_ = dialog;
    dialog->setObjectName(QStringLiteral("imagePreviewDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::ApplicationModal);
    connect(dialog, &QObject::destroyed, this, [this, dialog] {
        if (imagePreviewDialog_ == dialog) imagePreviewDialog_ = nullptr;
    });

    // 普通可缩放窗口只 show 一次。避免 macOS 最大化主窗口时进入原生全屏
    // Space，也避免 showFullScreen() + exec() 组合造成预览反复闪现。
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::openFilePreview(const RemoteIMFileAttachment& attachment) {
    // 非文档类型没有内嵌预览，转「另存为」。
    if (!isPreviewableDocument(attachment)) {
        saveFileAttachmentToLocal(attachment);
        return;
    }
    const QString displayName = attachment.fileName.isEmpty() ? QFileInfo(attachment.localPath).fileName() : attachment.fileName;
    const QString html = isHtmlFile(attachment)
        ? readTextFile(attachment.localPath)
        : UiZoom::scaleQss(MarkdownRenderer::renderToHtml(readTextFile(attachment.localPath)));
    FilePreviewDialog dialog(displayName, html, this);
    dialog.exec();
}

bool MainWindow::copyAttachmentToPath(const RemoteIMFileAttachment& attachment,
                                      const QString& targetPath,
                                      QString* errorMessage) {
    const auto fail = [errorMessage](const QString& reason) {
        if (errorMessage) *errorMessage = reason;
        return false;
    };
    const QString sourcePath = attachment.localPath.trimmed();
    if (sourcePath.isEmpty() || !QFile::exists(sourcePath)) {
        return fail(QStringLiteral("文件尚未下载完成或本地缓存已被清理。"));
    }
    if (targetPath.trimmed().isEmpty()) {
        return fail(QStringLiteral("保存路径为空。"));
    }
    if (QFileInfo(sourcePath).canonicalFilePath() == QFileInfo(targetPath).canonicalFilePath()) {
        return fail(QStringLiteral("目标位置与源文件相同。"));
    }
    // QFile::copy 遇到已存在的目标会失败；覆盖语义由调用方的「另存为」对话框确认过。
    if (QFile::exists(targetPath) && !QFile::remove(targetPath)) {
        return fail(QStringLiteral("无法覆盖已存在的文件：%1").arg(targetPath));
    }
    if (!QFile::copy(sourcePath, targetPath)) {
        return fail(QStringLiteral("写入失败：%1").arg(targetPath));
    }
    return true;
}

void MainWindow::saveFileAttachmentToLocal(const RemoteIMFileAttachment& attachment) {
    const QString suggestedName = attachment.fileName.isEmpty()
        ? QFileInfo(attachment.localPath).fileName()
        : attachment.fileName;
    QString startDir = lastAttachmentSaveDir_;
    if (startDir.isEmpty() || !QDir(startDir).exists()) {
        startDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    if (startDir.isEmpty()) startDir = QDir::homePath();
    const QString targetPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("保存到本地"),
        QDir(startDir).filePath(suggestedName.isEmpty() ? QStringLiteral("file") : suggestedName));
    if (targetPath.isEmpty()) return;  // 用户取消

    QString error;
    if (!copyAttachmentToPath(attachment, targetPath, &error)) {
        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning, QStringLiteral("保存失败"), error);
        return;
    }
    lastAttachmentSaveDir_ = QFileInfo(targetPath).absolutePath();
    AppMessageDialog::show(this, AppMessageDialog::Kind::Info, QStringLiteral("保存成功"),
                           QStringLiteral("已保存到：\n%1").arg(QDir::toNativeSeparators(targetPath)));
}

QWidget* MainWindow::createMessageBubble(const RemoteIMMessage& message) {
    const bool outgoing = message.direction == RemoteIMMessageDirection::Outgoing;
    auto* row = new QWidget(messageContainer_);
    row->setObjectName(outgoing ? QStringLiteral("messageRowOutgoing") : QStringLiteral("messageRowIncoming"));
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(UiZoom::s(MessageAvatarGap));

    auto* bubble = new QWidget(row);
    bubble->setObjectName(outgoing ? QStringLiteral("messageBubbleOutgoing") : QStringLiteral("messageBubbleIncoming"));
    const bool expandedTextBubble = !message.hasImage && !message.hasFile && !message.hasVideo && !message.hasVoice && (message.text.size() >= 50 || message.text.contains(QLatin1Char('\n')));
    bubble->setProperty("expandedTextBubble", expandedTextBubble);
    applyMessageBubbleWidth(bubble, expandedTextBubble);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // 配色/圆角对齐 Electron 端 .remote-im-bubble：本方(用户)白底 #dbeafe 边，
    // 对方(aicli)米黄底 #fffbeb + #fde68a 边，圆角 16px。
    bubble->setStyleSheet(UiZoom::scaleQss(outgoing
                              ? QStringLiteral("#messageBubbleOutgoing{background:#ffffff;border:1px solid #dbeafe;border-radius:16px;}")
                              : QStringLiteral("#messageBubbleIncoming{background:#fffbeb;border:1px solid #fde68a;border-radius:16px;}")));

    auto* bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(14, 11, 14, 12);
    bubbleLayout->setSpacing(7);

    // meta 行（作者/好友徽章/时间）放在气泡外部上方（飞书式），不与正文混在同一气泡里。
    auto* metaRow = new QHBoxLayout();
    metaRow->setContentsMargins(6, 0, 6, 0);
    metaRow->setSpacing(8);
    auto* authorLabel = new QLabel(message.fromUserId, row);
    authorLabel->setObjectName(QStringLiteral("messageAuthorLabel"));
    auto* timeLabel = new QLabel(messageTimeText(message), row);
    timeLabel->setObjectName(QStringLiteral("messageTimeLabel"));
    if (outgoing) metaRow->addStretch(1);
    metaRow->addWidget(authorLabel);
    if (!outgoing) {
        auto* relationLabel = new QLabel(QStringLiteral("好友"), row);
        relationLabel->setObjectName(QStringLiteral("messageRelationBadge"));
        metaRow->addWidget(relationLabel);
    }
    metaRow->addWidget(timeLabel);
    if (!outgoing) metaRow->addStretch(1);

    auto* contentRow = new QHBoxLayout();
    contentRow->setContentsMargins(0, 0, 0, 0);
    contentRow->setSpacing(10);
    QLabel* deliveryStatusLabel = nullptr;

    if (message.hasImage) {
        {
            auto* imageLabel = new ClickableImageLabel(message.image.localPath, [this](const QString& path) {
                openImagePreview(path);
            }, bubble);
            imageLabel->setObjectName(QStringLiteral("messageImageLabel"));
            // 高分屏（DPR>1）按物理分辨率解码并声明 DPR，否则缩略图被绘制层二次放大而发虚；
            // 控件尺寸用逻辑值（物理尺寸 / DPR）。
            const qreal thumbDpr = imageLabel->devicePixelRatioF();
            const QSize targetPixels = (QSizeF(UiZoom::s(280), UiZoom::s(200)) * thumbDpr).toSize();
            imageLabel->setAlignment(Qt::AlignCenter);
            // 先按目标尺寸占位，避免解码回来时气泡高度突变把列表顶得乱跳。
            imageLabel->setMinimumSize((QSizeF(targetPixels) / thumbDpr).toSize());
            // 解码放后台、按目标尺寸降采样、结果进缓存。原先这里是 QPixmap(path)
            // 解全尺寸原图再缩，实测 12MP 照片 46.8ms/张，且每次重建都重解。
            MessageImageLoader::instance().loadInto(
                message.image.localPath, targetPixels, imageLabel, [imageLabel, thumbDpr] {
                    imageLabel->setText(QStringLiteral("图片暂不可预览"));
                    imageLabel->setMinimumSize(QSize());
                    Q_UNUSED(thumbDpr);
                });
            // 右键菜单（飞书式）：复制图片 / 预览 / 保存到本地。
            imageLabel->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(imageLabel, &QLabel::customContextMenuRequested, this,
                    [this, imageLabel, image = message.image](const QPoint& pos) {
                QMenu menu(imageLabel);
                applyMessageContextMenuStyle(menu);
                QAction* copyAction = menu.addAction(makeLineIcon(LineIconKind::Copy, kMenuIconColor),
                                                     QStringLiteral("复制"));
                QAction* previewAction = menu.addAction(makeLineIcon(LineIconKind::Preview, kMenuIconColor),
                                                        QStringLiteral("预览"));
                menu.addSeparator();
                QAction* saveAction = menu.addAction(makeLineIcon(LineIconKind::Download, kMenuIconColor),
                                                     QStringLiteral("保存到本地…"));
                QAction* chosen = menu.exec(imageLabel->mapToGlobal(pos));
                if (chosen == copyAction) {
                    // 位图 + 文件 URL 一起放剪贴板：贴到聊天/文档得到图片，贴到资源管理器得到文件。
                    const QImage imageData(image.localPath);
                    if (imageData.isNull()) {
                        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                                               QStringLiteral("复制失败"),
                                               QStringLiteral("图片缓存已不存在，无法复制。"));
                        return;
                    }
                    auto* mimeData = new QMimeData();
                    mimeData->setImageData(imageData);
                    mimeData->setUrls({QUrl::fromLocalFile(image.localPath)});
                    QApplication::clipboard()->setMimeData(mimeData);
                } else if (chosen == previewAction) {
                    openImagePreview(image.localPath);
                } else if (chosen == saveAction) {
                    saveFileAttachmentToLocal(RemoteIMFileAttachment{
                        image.localPath, QFileInfo(image.localPath).fileName(), QString(), image.sizeBytes});
                }
            });
            contentRow->addWidget(imageLabel);
        }
    } else if (message.hasVoice) {
        // 语音气泡：时长 + 点击用系统播放器打开。腾讯 IM 的语音多是 AMR/SILK，
        // Qt Multimedia 在 Windows 上不一定解得了，交给系统关联程序更可靠。
        auto* voiceButton = new QPushButton(bubble);
        voiceButton->setObjectName(QStringLiteral("messageVoiceButton"));
        voiceButton->setCursor(Qt::PointingHandCursor);
        const int seconds = qMax(1, message.voice.durationSeconds);
        voiceButton->setText(QStringLiteral("🔊 语音 %1\"").arg(seconds));
        voiceButton->setMinimumWidth(UiZoom::s(120 + qMin(seconds, 30) * 4));
        voiceButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        voiceButton->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            QPushButton#messageVoiceButton {
                background: #f8fafc;
                border: 1px solid #d9e4ef;
                border-radius: 8px;
                color: #172033;
                font-size: 13px;
                padding: 10px 14px;
                text-align: left;
            }
            QPushButton#messageVoiceButton:hover {
                border-color: #1aa7ec;
                background: #edf8ff;
            }
        )")));
        connect(voiceButton, &QPushButton::clicked, this, [this, path = message.voice.localPath]() {
            if (path.trimmed().isEmpty() || !QFile::exists(path)) {
                AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                                       QStringLiteral("无法播放"),
                                       QStringLiteral("语音尚未下载完成或本地缓存已被清理。"));
                return;
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        contentRow->addWidget(voiceButton);
    } else if (message.hasVideo) {
        // 视频气泡：封面 + 中心播放角标，点击开播。封面拿不到（老消息、或生成失败）
        // 时退化成深色底 + 角标，仍然可点——不能因为没有封面就让视频打不开。
        auto* videoButton = new QPushButton(bubble);
        videoButton->setObjectName(QStringLiteral("messageVideoButton"));
        videoButton->setCursor(Qt::PointingHandCursor);
        videoButton->setFlat(true);

        constexpr int kCoverMaxWidth = 240;
        // 封面同样不在这里同步解码：先摆一张纯色占位，解码回来再把带角标的封面换上。
        const int coverWidth = UiZoom::s(kCoverMaxWidth);
        const int coverHeight = UiZoom::s(135);
        QPixmap cover(coverWidth, coverHeight);
        cover.fill(QColor(0x11, 0x18, 0x27));
        // 播放角标画进封面本身：QPushButton 的 icon 只有一层，叠控件在这里
        // 会被气泡的布局挤走。
        // 抽成 lambda：占位封面和随后解码回来的真封面都要画同一个角标。
        auto paintPlayBadge = [](QPixmap& target) {
            QPainter painter(&target);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QPointF center(target.width() / 2.0, target.height() / 2.0);
            const double radius = qMin(target.width(), target.height()) * 0.16;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 110));
            painter.drawEllipse(center, radius, radius);
            QPainterPath triangle;
            const double half = radius * 0.62;
            const double shift = half * 0.18;
            triangle.moveTo(center.x() - half * 0.55 + shift, center.y() - half);
            triangle.lineTo(center.x() - half * 0.55 + shift, center.y() + half);
            triangle.lineTo(center.x() + half * 0.85 + shift, center.y());
            triangle.closeSubpath();
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.drawPath(triangle);
        };
        paintPlayBadge(cover);
        videoButton->setIcon(QIcon(cover));
        videoButton->setIconSize(QSize(coverWidth, coverHeight));
        // 真封面后台解码，回来再换上；解不出来就一直是占位图，不影响播放。
        if (!message.video.coverPath.trimmed().isEmpty()) {
            const qreal coverDpr = videoButton->devicePixelRatioF();
            MessageImageLoader::instance().load(
                message.video.coverPath,
                (QSizeF(coverWidth, coverHeight) * coverDpr).toSize(),
                videoButton,
                [videoButton, coverWidth, coverHeight, paintPlayBadge](const QPixmap& loaded) {
                    QPixmap composed = loaded.scaled(QSize(coverWidth, coverHeight),
                                                     Qt::KeepAspectRatioByExpanding,
                                                     Qt::SmoothTransformation);
                    paintPlayBadge(composed);
                    videoButton->setIcon(QIcon(composed));
                });
        }
        videoButton->setFixedSize(coverWidth, coverHeight);
        videoButton->setStyleSheet(QStringLiteral(
            "QPushButton#messageVideoButton { border: none; padding: 0; background: transparent; }"));
        const QString durationText = message.video.durationSeconds > 0
            ? QStringLiteral("%1:%2")
                  .arg(message.video.durationSeconds / 60, 2, 10, QLatin1Char('0'))
                  .arg(message.video.durationSeconds % 60, 2, 10, QLatin1Char('0'))
            : QString();
        const QString sizeText = fileSizeText(message.video.sizeBytes);
        QStringList parts;
        if (!durationText.isEmpty()) parts << durationText;
        if (!sizeText.isEmpty()) parts << sizeText;
        videoButton->setToolTip(parts.isEmpty()
            ? QStringLiteral("点击播放")
            : QStringLiteral("点击播放 · %1").arg(parts.join(QStringLiteral(" · "))));
        connect(videoButton, &QPushButton::clicked, this,
                [this, attachment = message.video]() { openVideoPreview(attachment); });
        contentRow->addWidget(videoButton);
        if (!parts.isEmpty()) {
            auto* metaLabel = new QLabel(parts.join(QStringLiteral(" · ")), bubble);
            metaLabel->setStyleSheet(QStringLiteral("color: #6b7a8c; font-size: 12px;"));
            contentRow->addWidget(metaLabel);
        }
    } else if (message.hasFile) {
        auto* fileButton = new QPushButton(bubble);
        fileButton->setObjectName(QStringLiteral("messageFileButton"));
        fileButton->setCursor(Qt::PointingHandCursor);
        const QString displayName = message.file.fileName.isEmpty()
            ? QFileInfo(message.file.localPath).fileName()
            : message.file.fileName;
        QString subtitle;
        QString icon = QStringLiteral("📄");
        if (isHtmlFile(message.file)) {
            subtitle = QStringLiteral("HTML 文件，点击预览");
        } else if (isMarkdownFile(message.file)) {
            subtitle = QStringLiteral("Markdown 文件，点击预览");
        } else if (isVideoFile(message.file)) {
            icon = QStringLiteral("🎬");
            const QString size = fileSizeText(message.file.sizeBytes);
            subtitle = size.isEmpty() ? QStringLiteral("视频，点击另存为")
                                      : QStringLiteral("视频 · %1，点击另存为").arg(size);
        } else {
            // 普通文件：无内嵌预览，点击直接另存为；有大小时一并展示。
            const QString size = fileSizeText(message.file.sizeBytes);
            subtitle = size.isEmpty() ? QStringLiteral("文件，点击另存为")
                                      : QStringLiteral("文件 · %1，点击另存为").arg(size);
        }
        fileButton->setText(QStringLiteral("%1 %2\n%3")
            .arg(icon)
            .arg(displayName.isEmpty() ? QStringLiteral("file") : displayName)
            .arg(subtitle));
        fileButton->setMinimumWidth(UiZoom::s(220));
        fileButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        fileButton->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
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
        )")));
        connect(fileButton, &QPushButton::clicked, this, [this, attachment = message.file]() {
            if (isPreviewableDocument(attachment)) {
                openFilePreview(attachment);
            } else {
                saveFileAttachmentToLocal(attachment);
            }
        });
        // 右键菜单（飞书式）：复制文件 / 预览（仅文档） / 保存到本地。
        fileButton->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(fileButton, &QPushButton::customContextMenuRequested, this,
                [this, fileButton, attachment = message.file](const QPoint& pos) {
            QMenu menu(fileButton);
            applyMessageContextMenuStyle(menu);
            QAction* copyAction = menu.addAction(makeLineIcon(LineIconKind::Copy, kMenuIconColor),
                                                 QStringLiteral("复制"));
            QAction* previewAction = isPreviewableDocument(attachment)
                ? menu.addAction(makeLineIcon(LineIconKind::Preview, kMenuIconColor),
                                 QStringLiteral("预览"))
                : nullptr;
            menu.addSeparator();
            QAction* saveAction = menu.addAction(makeLineIcon(LineIconKind::Download, kMenuIconColor),
                                                 QStringLiteral("保存到本地…"));
            QAction* chosen = menu.exec(fileButton->mapToGlobal(pos));
            if (chosen == copyAction) {
                // 以文件形式放剪贴板：可直接粘贴到资源管理器/聊天窗口。
                if (attachment.localPath.trimmed().isEmpty() || !QFile::exists(attachment.localPath)) {
                    AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                                           QStringLiteral("复制失败"),
                                           QStringLiteral("文件尚未下载完成或本地缓存已被清理。"));
                    return;
                }
                auto* mimeData = new QMimeData();
                mimeData->setUrls({QUrl::fromLocalFile(attachment.localPath)});
                QApplication::clipboard()->setMimeData(mimeData);
            } else if (previewAction && chosen == previewAction) {
                openFilePreview(attachment);
            } else if (chosen == saveAction) {
                saveFileAttachmentToLocal(attachment);
            }
        });
        contentRow->addWidget(fileButton);
    } else {
        auto* markdownView = new MarkdownMessageView(bubble);
        markdownView->setMessageMarkdown(message.text);
        contentRow->addWidget(markdownView, 1);
    }

    const QString status = outgoing ? deliveryStatusIndicator(message.status) : QString();
    if (!status.isEmpty()) {
        deliveryStatusLabel = new QLabel(status, row);
        deliveryStatusLabel->setObjectName(QStringLiteral("messageStatusLabel"));
        deliveryStatusLabel->setAlignment(Qt::AlignCenter);
        deliveryStatusLabel->setFixedSize(UiZoom::s(16), UiZoom::s(16));
        deliveryStatusLabel->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            QLabel#messageStatusLabel {
                border: 1px solid #12a150;
                border-radius: 8px;
                background: transparent;
                color: #12a150;
                font-size: 11px;
                font-weight: 800;
                padding: 0;
            }
        )")));
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
    // 样式挂在 row 上（meta 已移出气泡，不再随气泡背景）。
    row->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
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
        /* 搜索命中高亮：用动态属性切换，不去改写 row 的 styleSheet——
           这份样式表还带着上面几条 meta 规则，整体替换会把它们一起抹掉。 */
        #messageRowOutgoing[searchHit="true"], #messageRowIncoming[searchHit="true"] {
            background: #fff4c2;
            border-radius: 10px;
        }
    )")));

    // meta 在上、气泡在下的纵向列；随消息方向靠左/靠右。
    auto* column = new QVBoxLayout();
    // Let the avatar center sit midway between the metadata line's center and the
    // bubble's top border. The meta row height depends on platform font metrics
    // (Windows renders 13px labels shorter than macOS), so a fixed top inset cannot
    // align both — solve 2*inset + 1.5*metaHeight + gap = avatarSize instead.
    const int metaHeight = qMax(1, metaRow->sizeHint().height());
    const int metaBubbleGap = UiZoom::s(MessageMetaBubbleGap);
    const int columnTopInset = qMax(
        0, (UiZoom::s(MessageAvatarLogicalSize) - metaBubbleGap) / 2 - (metaHeight * 3) / 4);
    column->setContentsMargins(0, columnTopInset, 0, 0);
    column->setSpacing(metaBubbleGap);
    column->addLayout(metaRow);
    auto* bubbleRow = new QHBoxLayout();
    bubbleRow->setContentsMargins(0, 0, 0, 0);
    bubbleRow->setSpacing(UiZoom::s(8));
    bubbleRow->setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
    bubbleRow->addWidget(bubble);
    if (deliveryStatusLabel) {
        bubbleRow->addWidget(deliveryStatusLabel, 0, Qt::AlignVCenter);
    }
    column->addLayout(bubbleRow);

    const QString avatarUserId = message.fromUserId.trimmed();
    const QString avatarDisplayName = contactName(avatarUserId);
    QString avatarUrl;
    for (const RemoteIMContact& contact : app_.chatState().contacts()) {
        if (contact.userId == avatarUserId) {
            avatarUrl = contact.avatarUrl;
            break;
        }
    }
    auto* avatar = createMessageAvatarLabel(
        avatarUserId, avatarDisplayName, avatarUrl, outgoing, row);
    if (outgoing) {
        rowLayout->addStretch(1);
        rowLayout->addLayout(column);
        rowLayout->addWidget(avatar, 0, Qt::AlignTop);
    } else {
        rowLayout->addWidget(avatar, 0, Qt::AlignTop);
        rowLayout->addLayout(column);
        rowLayout->addStretch(1);
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
    const QMargins margins = messageLayout_ ? messageLayout_->contentsMargins() : QMargins();
    const int rowWidth = viewportWidth - margins.left() - margins.right();
    const int avatarColumnWidth = UiZoom::s(MessageAvatarLogicalSize)
        + UiZoom::s(MessageAvatarGap);
    // Reserve a sender-avatar column at both ends of every row. The bubble starts
    // after its own avatar and stops before the opposite sender's avatar; changing
    // avatar size or UI zoom therefore updates the limit automatically.
    return qBound(280, rowWidth - 2 * avatarColumnWidth, 1280);
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
    row->setMinimumHeight(UiZoom::s(72));
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
        valueLabel->setMinimumWidth(UiZoom::s(180));
        // 供 applyScaledFixedGeometry 在倍率变化时重放最小宽度。
        valueLabel->setProperty("settingsRowValue", true);
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
            switch (attachments.at(i).kind) {
            case ComposerAttachment::Kind::File:
                app_.sendFile(attachments.at(i).path, caption);
                break;
            case ComposerAttachment::Kind::Video:
                app_.sendVideo(attachments.at(i).path, caption);
                break;
            case ComposerAttachment::Kind::Image:
                app_.sendImage(attachments.at(i).path, caption);
                break;
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
        button->setFixedHeight(UiZoom::s(kSlashCommandRowHeight));
        button->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
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
        )")));
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
    const int barHeightForRows = visibleRows * UiZoom::s(kSlashCommandRowHeight)
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

void MainWindow::changeUiZoom(qreal delta) {
    UiZoom::setFactor(UiZoom::factor() + delta);
    applyUiZoom(true);
}

void MainWindow::resetUiZoom() {
    UiZoom::setFactor(1.0);
    applyUiZoom(true);
}

void MainWindow::applyUiZoom(bool showToastPopup) {
    // 全局默认字体随倍率缩放（基准 13px，与 main.cpp 启动设置一致）。
    QFont font = QApplication::font();
    font.setPixelSize(UiZoom::s(13));
    QApplication::setFont(font);
    // 全局样式表按新倍率重放；列表条目行高/头像与气泡样式都依赖倍率，
    // 清空渲染缓存强制消息全量重建。代码级最小宽高也须重放（否则缩不回去）。
    applyScaledFixedGeometry();
    applyStyle();
    renderedPeerId_.clear();
    refresh();
    if (showToastPopup) showZoomToast();
}

void MainWindow::applyScaledFixedGeometry() {
    setMinimumSize(UiZoom::s(980), UiZoom::s(640));
    if (navRail_) {
        navRail_->setMinimumWidth(UiZoom::s(160));
        navRail_->setMaximumWidth(UiZoom::s(260));
    }
    // navLogo 是生成位图（QPainterPath 灰度抗锯齿），倍率变化时按新尺寸重生成。
    if (auto* logo = findChild<QLabel*>(QStringLiteral("navLogo"))) {
        logo->setPixmap(monogramAvatarPixmap(QStringLiteral("M"), UiZoom::s(34), UiZoom::s(17),
                                             kBrandGradientFrom, kBrandGradientTo,
                                             UiZoom::s(15), logo->devicePixelRatioF()));
    }
    if (auto* pane = findChild<QWidget*>(QStringLiteral("conversationPane"))) {
        pane->setMinimumWidth(UiZoom::s(220));
    }
    if (auto* pane = findChild<QWidget*>(QStringLiteral("chatContentPane"))) {
        pane->setMinimumWidth(UiZoom::s(520));
    }
    if (auto* pane = findChild<QWidget*>(QStringLiteral("composerPanel"))) {
        pane->setMinimumHeight(UiZoom::s(116));
    }
    if (auto* pane = findChild<QWidget*>(QStringLiteral("contactsDirectoryPane"))) {
        pane->setMinimumWidth(UiZoom::s(300));
        pane->setMaximumWidth(UiZoom::s(420));
    }
    if (messageEditor_) messageEditor_->setMinimumHeight(UiZoom::s(64));
    if (sendButton_) {
        sendButton_->setFixedSize(UiZoom::s(36), UiZoom::s(36));
        sendButton_->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
        static_cast<ComposerTextEdit*>(messageEditor_)->positionCornerAction();
    }
    const QList<QWidget*> settingsRows = findChildren<QWidget*>(QStringLiteral("settingsRow"));
    for (QWidget* row : settingsRows) row->setMinimumHeight(UiZoom::s(72));
    const QList<QLabel*> labels = findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->property("settingsRowValue").toBool()) label->setMinimumWidth(UiZoom::s(180));
    }
}

void MainWindow::showZoomToast() {
    if (!zoomToast_) {
        zoomToast_ = new QLabel(this);
        zoomToast_->setObjectName(QStringLiteral("zoomToast"));
        zoomToast_->setAlignment(Qt::AlignCenter);
        zoomToast_->setAttribute(Qt::WA_TransparentForMouseEvents);
        zoomToastTimer_ = new QTimer(this);
        zoomToastTimer_->setSingleShot(true);
        connect(zoomToastTimer_, &QTimer::timeout, this, [this] { zoomToast_->hide(); });
    }
    zoomToast_->setText(QStringLiteral("%1%").arg(qRound(UiZoom::factor() * 100)));
    zoomToast_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QLabel#zoomToast {
            background: rgba(17, 24, 39, 0.88);
            color: #ffffff;
            border-radius: 10px;
            font-size: 18px;
            font-weight: 800;
            padding: 12px 26px;
        }
    )")));
    zoomToast_->adjustSize();
    zoomToast_->move((width() - zoomToast_->width()) / 2, (height() - zoomToast_->height()) / 2);
    zoomToast_->raise();
    zoomToast_->show();
    zoomToastTimer_->start(900);
}

void MainWindow::updateRemoteDesktopButton() {
    if (!remoteDesktopButton_) return;

    const QString peerId = app_.chatState().selectedPeerId();
    const bool trtcReady = RemoteDesktop::isTrtcAvailable();
    const RemoteDesktop::ViewerState state =
        remoteDesktop_ ? remoteDesktop_->viewerState() : RemoteDesktop::ViewerState::Idle;

    // 同一个按钮承载三态：发起 / 连接中 / 断开。会话进行中时按钮即出口，
    // 观看窗被最小化也能从主界面掐断。
    LineIconKind icon = LineIconKind::Screen;
    QColor color(QStringLiteral("#4c5866"));
    QString tooltip;
    bool enabled = trtcReady && !peerId.isEmpty();

    switch (state) {
        case RemoteDesktop::ViewerState::Inviting:
        case RemoteDesktop::ViewerState::Connecting:
            icon = LineIconKind::ScreenConnecting;
            color = QColor(QStringLiteral("#b45309"));
            tooltip = QStringLiteral("正在连接远程桌面…点击取消");
            enabled = true;
            break;
        case RemoteDesktop::ViewerState::Viewing:
            icon = LineIconKind::ScreenDisconnect;
            color = QColor(QStringLiteral("#b42318"));
            tooltip = QStringLiteral("远程桌面已连接 · 点击断开");
            enabled = true;
            break;
        case RemoteDesktop::ViewerState::Idle:
        case RemoteDesktop::ViewerState::Failed:
            // 不可用时用 tooltip 说清原因，避免用户对着灰按钮猜。
            if (!trtcReady) {
                tooltip = QStringLiteral("当前版本未包含远程桌面组件");
            } else if (peerId.isEmpty()) {
                tooltip = QStringLiteral("请先选择一个会话");
            } else {
                tooltip = QStringLiteral("远程桌面 · 请求查看 %1 的屏幕").arg(peerId);
            }
            break;
    }

    remoteDesktopButton_->setIcon(makeLineIcon(icon, color));
    remoteDesktopButton_->setToolTip(tooltip);
    remoteDesktopButton_->setEnabled(enabled);
}

QWidget* MainWindow::buildRemoteDesktopSettingsPanel(QWidget* parent) {
    auto* panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("settingsPanel"));
    auto* layout = new QVBoxLayout(panel);
    // 同上：上下留白，左右保持 0 以便分隔线横贯整宽。
    layout->setContentsMargins(0, UiZoom::s(8), 0, UiZoom::s(8));
    layout->setSpacing(0);

    auto* heading = new QLabel(QStringLiteral("远程桌面"), panel);
    heading->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(heading);

    // 被控模式三选一。默认无人值守：主场景是自己在外面连自己的电脑，
    // 每次都要人在电脑前点同意就失去意义了。
    auto* modeRow = new QWidget(panel);
    modeRow->setObjectName(QStringLiteral("settingsRow"));
    auto* modeLayout = new QVBoxLayout(modeRow);
    modeLayout->setContentsMargins(18, 14, 18, 14);
    modeLayout->setSpacing(8);

    auto* modeTitle = new QLabel(QStringLiteral("被控模式"), modeRow);
    modeTitle->setObjectName(QStringLiteral("settingsRowTitle"));
    modeLayout->addWidget(modeTitle);

    remoteDesktopModeGroup_ = new QButtonGroup(this);
    struct ModeOption {
        RemoteDesktop::HostMode mode;
        QString label;
        QString hint;
    };
    const QVector<ModeOption> options{
        {RemoteDesktop::HostMode::Unattended, QStringLiteral("无人值守"),
         QStringLiteral("允许列表内的设备可直接连入，不打扰你")},
        {RemoteDesktop::HostMode::Attended, QStringLiteral("每次确认"),
         QStringLiteral("每次收到请求都弹窗，60 秒无应答自动拒绝")},
        {RemoteDesktop::HostMode::Disabled, QStringLiteral("关闭"),
         QStringLiteral("拒绝一切远程请求")}};
    for (const ModeOption& option : options) {
        auto* radio = new QRadioButton(
            QStringLiteral("%1 · %2").arg(option.label, option.hint), modeRow);
        radio->setObjectName(QStringLiteral("settingsRadio"));
        radio->setCursor(Qt::PointingHandCursor);
        remoteDesktopModeGroup_->addButton(radio, static_cast<int>(option.mode));
        modeLayout->addWidget(radio);
    }
    connect(remoteDesktopModeGroup_,
            QOverload<int>::of(&QButtonGroup::buttonClicked), this,
            [this](int id) {
                RemoteDesktopSettings settings = remoteDesktop_->settings();
                settings.mode = static_cast<RemoteDesktop::HostMode>(id);
                // 换模式即视为用户明确表态，清掉此前的失败计数。
                settings.consecutiveAuthFailures = 0;
                remoteDesktop_->updateSettings(settings);
                remoteDesktopSettingsStore_->save(settings);
                refreshRemoteDesktopSettings();
            });
    layout->addWidget(modeRow);

    // 访问密码：可选加固，不是无人值守的前提。
    remoteDesktopPasswordValue_ = new QLabel(panel);
    remoteDesktopPasswordValue_->setObjectName(QStringLiteral("settingsRowValue"));
    remoteDesktopPasswordValue_->setProperty("settingsRowValue", true);
    auto* passwordRow = createSettingsRow(
        QStringLiteral("访问密码"), remoteDesktopPasswordValue_,
        QStringLiteral("可选。设置后，对方连入时还需输入此密码；不设则仅凭允许列表授权。"));
    auto* passwordButton = new QPushButton(QStringLiteral("设置"), passwordRow);
    passwordButton->setObjectName(QStringLiteral("settingsRowButton"));
    passwordButton->setCursor(Qt::PointingHandCursor);
    connect(passwordButton, &QPushButton::clicked, this,
            &MainWindow::editRemoteDesktopPassword);
    if (auto* rowLayout = qobject_cast<QHBoxLayout*>(passwordRow->layout())) {
        rowLayout->addWidget(passwordButton);
    }
    layout->addWidget(passwordRow);

    // 允许列表：谁能连入本机。
    remoteDesktopAllowValue_ = new QLabel(panel);
    remoteDesktopAllowValue_->setObjectName(QStringLiteral("settingsRowValue"));
    remoteDesktopAllowValue_->setProperty("settingsRowValue", true);
    auto* allowRow = createSettingsRow(
        QStringLiteral("允许连入的设备"), remoteDesktopAllowValue_,
        QStringLiteral("只有列表内的账号能远程本机，其余一律拒绝。"));
    auto* allowButton = new QPushButton(QStringLiteral("编辑"), allowRow);
    allowButton->setObjectName(QStringLiteral("settingsRowButton"));
    allowButton->setCursor(Qt::PointingHandCursor);
    connect(allowButton, &QPushButton::clicked, this, &MainWindow::editRemoteDesktopAllowList);
    if (auto* rowLayout = qobject_cast<QHBoxLayout*>(allowRow->layout())) {
        rowLayout->addWidget(allowButton);
    }
    layout->addWidget(allowRow);

    // 远程控制：与「允许观看」彼此独立的一道闸，默认关。
    // 不跟着无人值守走——否则那一个开关会在用户不知情的情况下，
    // 把语义从"允许别人看我的屏幕"放大成"允许别人完全操作我的电脑"。
    remoteDesktopControlValue_ = new QLabel(panel);
    remoteDesktopControlValue_->setObjectName(QStringLiteral("settingsRowValue"));
    remoteDesktopControlValue_->setProperty("settingsRowValue", true);
    QString remoteControlHelp =
        QStringLiteral("关闭时对方只能看画面，动不了你的鼠标键盘。开启后无需每次确认——"
                       "人不在电脑前时弹窗没人应答，等于让无人值守失效。");
#ifdef Q_OS_MAC
    remoteControlHelp +=
        QStringLiteral(" macOS 首次开启还需在“隐私与安全性 > 辅助功能”中允许 MaiChat。");
#endif
    auto* controlRow = createSettingsRow(QStringLiteral("允许远程控制"),
                                         remoteDesktopControlValue_, remoteControlHelp);
    remoteDesktopControlToggle_ = new QCheckBox(QStringLiteral("允许"), controlRow);
    remoteDesktopControlToggle_->setObjectName(QStringLiteral("remoteControlToggle"));
    const bool canInjectInput = RemoteInput::isInputInjectionSupported();
    remoteDesktopControlToggle_->setEnabled(canInjectInput);
    remoteDesktopControlToggle_->setCursor(canInjectInput ? Qt::PointingHandCursor
                                                          : Qt::ArrowCursor);
    connect(remoteDesktopControlToggle_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!RemoteInput::isInputInjectionSupported()) return;
        RemoteDesktopSettings settings = remoteDesktop_->settings();
        if (settings.allowRemoteControl == checked) return;
        settings.allowRemoteControl = checked;
        remoteDesktop_->updateSettings(settings);
        remoteDesktopSettingsStore_->save(settings);
        if (checked && !RemoteInput::hasInputInjectionPermission()) {
            RemoteInput::requestInputInjectionPermission();
            AppMessageDialog::show(
                this, AppMessageDialog::Kind::Info, QStringLiteral("需要辅助功能权限"),
                QStringLiteral("请在“系统设置 > 隐私与安全性 > 辅助功能”中允许 MaiChat。"
                               "授权后返回本应用即可接受远程鼠标和键盘操作。"));
        }
        refreshRemoteDesktopSettings();
    });
    if (auto* rowLayout = qobject_cast<QHBoxLayout*>(controlRow->layout())) {
        rowLayout->addWidget(remoteDesktopControlToggle_);
    }
    layout->addWidget(controlRow);

    remoteDesktopProxyValue_ = new QLabel(panel);
    remoteDesktopProxyValue_->setObjectName(QStringLiteral("settingsRowValue"));
    remoteDesktopProxyValue_->setProperty("settingsRowValue", true);
    auto* proxyRow = createSettingsRow(
        QStringLiteral("TRTC 网络代理"), remoteDesktopProxyValue_,
        QStringLiteral("为远程桌面单独设置 SOCKS5，不影响 IM。保存后重启 MaiChat 生效。"));
    auto* proxyButton = new QPushButton(QStringLiteral("配置"), proxyRow);
    proxyButton->setObjectName(QStringLiteral("settingsRowButton"));
    proxyButton->setCursor(Qt::PointingHandCursor);
    connect(proxyButton, &QPushButton::clicked, this, &MainWindow::editRemoteDesktopProxy);
    if (auto* rowLayout = qobject_cast<QHBoxLayout*>(proxyRow->layout())) {
        rowLayout->addWidget(proxyButton);
    }
    layout->addWidget(proxyRow);

    return panel;
}

void MainWindow::refreshRemoteDesktopSettings() {
    if (!remoteDesktop_ || !remoteDesktopModeGroup_) return;
    const RemoteDesktopSettings& settings = remoteDesktop_->settings();

    if (auto* button = remoteDesktopModeGroup_->button(static_cast<int>(settings.mode))) {
        button->setChecked(true);
    }
    remoteDesktopPasswordValue_->setText(settings.hasPassword() ? QStringLiteral("已设置")
                                                                : QStringLiteral("未设置"));
    remoteDesktopAllowValue_->setText(settings.allowedUserIds.isEmpty()
                                          ? QStringLiteral("尚未添加")
                                          : settings.allowedUserIds.join(QStringLiteral("、")));

    if (remoteDesktopControlToggle_) {
        // 回填时挡掉 toggled：否则会反过来触发一次保存，形成回环。
        QSignalBlocker blocker(remoteDesktopControlToggle_);
        remoteDesktopControlToggle_->setChecked(
            RemoteInput::isInputInjectionSupported() && settings.allowRemoteControl);
    }
    if (remoteDesktopControlValue_) {
        QString status = QStringLiteral("仅可观看");
        if (!RemoteInput::isInputInjectionSupported()) {
            status = QStringLiteral("当前平台暂不支持");
        } else if (settings.allowRemoteControl
                   && !RemoteInput::hasInputInjectionPermission()) {
            status = QStringLiteral("等待辅助功能授权");
        } else if (settings.allowRemoteControl) {
            status = QStringLiteral("已允许");
        }
        remoteDesktopControlValue_->setText(status);
    }
    if (remoteDesktopProxyValue_) {
        remoteDesktopProxyValue_->setText(
            settings.trtcProxyEnabled
                ? QStringLiteral("%1:%2 · %3")
                      .arg(settings.trtcProxyHost)
                      .arg(settings.trtcProxyPort)
                      .arg(settings.trtcProxyUdp ? QStringLiteral("TCP + UDP")
                                                 : QStringLiteral("仅 TCP"))
                : QStringLiteral("直连"));
    }
}

void MainWindow::editRemoteDesktopPassword() {
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("访问密码");
    options.description =
        QStringLiteral("留空表示不设密码，仅凭允许列表授权。设置后对方连入时还需输入此密码。");
    options.placeholder = QStringLiteral("留空即不设密码");
    options.password = true;
    bool ok = false;
    const QString password = AppTextInputDialog::getText(this, options, &ok);
    if (!ok) return;

    RemoteDesktopSettings settings = remoteDesktop_->settings();
    settings.secret = password.isEmpty()
                          ? RemoteDesktopAuth::StoredSecret{}
                          : RemoteDesktopAuth::deriveSecret(
                                password, settings.secret.salt.isEmpty()
                                              ? RemoteDesktopAuth::generateSalt()
                                              : settings.secret.salt);
    settings.consecutiveAuthFailures = 0;
    remoteDesktop_->updateSettings(settings);
    remoteDesktopSettingsStore_->save(settings);
    refreshRemoteDesktopSettings();
}

void MainWindow::editRemoteDesktopAllowList() {
    const RemoteDesktopSettings current = remoteDesktop_->settings();
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("允许连入的设备");
    options.description = QStringLiteral("只有列表内的账号能远程本机，多个账号用逗号分隔。");
    options.placeholder = QStringLiteral("例如：whq-iphone, mac-air");
    options.initialText = current.allowedUserIds.join(QStringLiteral(","));
    bool ok = false;
    const QString text = AppTextInputDialog::getText(this, options, &ok);
    if (!ok) return;

    RemoteDesktopSettings settings = current;
    settings.allowedUserIds.clear();
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,，]")),
                                         Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString clean = part.trimmed();
        if (!clean.isEmpty()) settings.allowedUserIds.append(clean);
    }
    remoteDesktop_->updateSettings(settings);
    remoteDesktopSettingsStore_->save(settings);
    refreshRemoteDesktopSettings();
}

void MainWindow::editRemoteDesktopProxy() {
    const RemoteDesktopSettings current = remoteDesktop_->settings();
    RemoteDesktopProxyDialog::Config dialogConfig;
    dialogConfig.enabled = current.trtcProxyEnabled;
    dialogConfig.host = current.trtcProxyHost;
    dialogConfig.port = current.trtcProxyPort;
    dialogConfig.supportUdp = current.trtcProxyUdp;
    RemoteDesktopProxyDialog dialog(dialogConfig, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const RemoteDesktopProxyDialog::Config proxy = dialog.config();

    RemoteDesktopSettings settings = current;
    settings.trtcProxyEnabled = proxy.enabled;
    settings.trtcProxyHost =
        proxy.host.isEmpty() ? QStringLiteral("127.0.0.1") : proxy.host;
    settings.trtcProxyPort = proxy.port;
    settings.trtcProxyUdp = proxy.supportUdp;
    if (!remoteDesktopSettingsStore_->save(settings)) {
        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                               QStringLiteral("保存失败"),
                               QStringLiteral("无法保存 TRTC 网络代理设置。"));
        return;
    }
    remoteDesktop_->updateSettings(settings);
    refreshRemoteDesktopSettings();
    AppMessageDialog::show(this, AppMessageDialog::Kind::Info,
                           QStringLiteral("代理设置已保存"),
                           QStringLiteral("重启 MaiChat 后，TRTC 将使用新的网络代理。"));
}

void MainWindow::setupRemoteDesktop() {
    // 配置与本地消息库同级：每账号一份。
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
    const QString ownerId = app_.chatState().ownerUserId();
    remoteDesktopSettingsStore_ = std::make_unique<RemoteDesktopSettingsStore>(
        QDir(root).filePath(QStringLiteral("RemoteDesktop/") + ownerId
                            + QStringLiteral("/settings.json")));

    RemoteDesktopController::Config config;
    config.sdkAppId = RemoteIMCredentialDefaults::sdkAppId;
    config.localUserId = ownerId;
    config.userSigProvider = [](const QString& userId) {
        return TencentUserSigGenerator::generate(RemoteIMCredentialDefaults::sdkAppId, userId,
                                                 RemoteIMCredentialDefaults::secretKey());
    };

    RemoteDesktopSettings remoteSettings = remoteDesktopSettingsStore_->load();
    if (!RemoteInput::isInputInjectionSupported()) remoteSettings.allowRemoteControl = false;
    RemoteDesktop::TrtcNetworkProxyConfig proxyConfig;
    proxyConfig.enabled = remoteSettings.trtcProxyEnabled;
    proxyConfig.host = remoteSettings.trtcProxyHost;
    proxyConfig.port = remoteSettings.trtcProxyPort;
    proxyConfig.supportUdp = remoteSettings.trtcProxyUdp;
    auto trtcEngine =
        std::unique_ptr<RemoteDesktop::ITrtcEngine>(RemoteDesktop::createTrtcEngine(proxyConfig));
    const QString trtcInitializationError = trtcEngine->initializationError();
    remoteDesktop_ = new RemoteDesktopController(
        config, remoteSettings, std::move(trtcEngine),
        [this](const QString& peerId,
               const QString& text,
               RemoteDesktopController::SignalSendCompletion completion) {
            // Remote-desktop protocol frames are generated by the program, not
            // human chat input; receivers must never auto-reply to them via IM.
            app_.client().sendMachineText(
                peerId, text,
                [completion = std::move(completion)](
                    bool, const QString&, const RemoteIMSendReceipt&) mutable {
                    if (completion) completion();
                });
        },
        this);
    if (!trtcInitializationError.isEmpty()) {
        QTimer::singleShot(0, this, [this, trtcInitializationError] {
            AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                                   QStringLiteral("TRTC 初始化失败"),
                                   trtcInitializationError);
        });
    }

    connect(&app_, &RemoteIMApplication::remoteDesktopSignalReceived, this,
            [this](const QString& fromUserId, const QString& text) {
                remoteDesktop_->handleIncomingText(fromUserId, text);
            });

    connect(remoteDesktop_, &RemoteDesktopController::consentRequested, this,
            [this](const QString& fromUserId) { handleRemoteDesktopConsent(fromUserId); });

    connect(remoteDesktop_, &RemoteDesktopController::sharingStarted, this,
            [this](const QString& peerUserId) { sharingIndicator_->startSharing(peerUserId); });
    connect(remoteDesktop_, &RemoteDesktopController::sharingStopped, this,
            [this] { sharingIndicator_->stopSharing(); });
    connect(sharingIndicator_, &SharingIndicatorBar::stopRequested, this,
            [this] { remoteDesktop_->stopSession(); });

    // 控制器改写设置（失败计数、模式降级）后立即落盘，避免重启后计数丢失。
    connect(remoteDesktop_, &RemoteDesktopController::settingsChanged, this,
            [this](const RemoteDesktopSettings& settings) {
                remoteDesktopSettingsStore_->save(settings);
            });
    connect(remoteDesktop_, &RemoteDesktopController::modeDowngraded, this, [this] {
        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning, QStringLiteral("远程桌面"),
                               QStringLiteral("访问密码连续校验失败次数过多，"
                                              "无人值守已自动关闭，现在改为每次弹窗确认。"));
    });

    connect(remoteDesktop_, &RemoteDesktopController::viewerStateChanged, this,
            [this](RemoteDesktop::ViewerState state, const QString& failureReason) {
                // 按钮三态跟着会话状态走，必须先刷新再处理窗口开关。
                updateRemoteDesktopButton();
                switch (state) {
                    case RemoteDesktop::ViewerState::Connecting:
                        // 对方已同意：先把观看窗开出来，渲染句柄要在窗口显示后才有效。
                        {
                            const QString peerUserId = remoteDesktop_->viewerPeerId();
                            openRemoteDesktopViewer(peerUserId);
                            // 已知对端 userId 时无需等待辅流通知。下一轮事件循环
                            // 再绑定，确保 startViewing 已发起且 NSView/HWND 已创建。
                            QTimer::singleShot(0, this, [this, peerUserId] {
                                if (!remoteDesktop_ || !remoteDesktopView_
                                    || peerUserId.isEmpty()
                                    || !remoteDesktopView_->isSessionVisible()) {
                                    return;
                                }
                                const auto state = remoteDesktop_->viewerState();
                                if (state != RemoteDesktop::ViewerState::Connecting
                                    && state != RemoteDesktop::ViewerState::Viewing) {
                                    return;
                                }
                                remoteDesktop_->bindRemoteView(
                                    peerUserId,
                                    remoteDesktopView_->renderWindowHandle(peerUserId));
                            });
                        }
                        break;
                    case RemoteDesktop::ViewerState::Failed:
                        closeRemoteDesktopViewer();
                        if (failureReason == RemoteDesktop::reasonBadPassword()) {
                            // 只有对方额外设了密码才会走到这里：此时才问，问一次记住。
                            promptRemoteDesktopPassword(remoteDesktop_->viewerPeerId());
                        } else if (!failureReason.isEmpty()) {
                            AppMessageDialog::show(this, AppMessageDialog::Kind::Info, QStringLiteral("远程桌面"), failureReason);
                        }
                        break;
                    case RemoteDesktop::ViewerState::Idle:
                        closeRemoteDesktopViewer();
                        break;
                    case RemoteDesktop::ViewerState::Inviting:
                        // 新会话不能沿用上一场的编码帧尺寸。首帧/尺寸回调前宁可
                        // 暂停坐标输入，也不能拿固定 1920x1080 短暂映射错。
                        remoteDesktopRemoteVideoSize_ = QSize();
                        if (remoteInputCapture_) {
                            remoteInputCapture_->setRemoteVideoSize(QSize());
                        }
                        break;
                    case RemoteDesktop::ViewerState::Viewing:
                        break;
                }
            });

    // 辅流状态变化时刷新 UI 并重申绑定；首次订阅已在 Connecting 时主动发起，
    // 不依赖这条可能早于原生窗口创建的单次通知。
    remoteDesktop_->setRemoteVideoHandler(
        [this](const QString& userId, bool available) {
            if (!remoteDesktopView_) return;
            const QString peerUserId =
                userId.isEmpty() ? remoteDesktop_->viewerPeerId() : userId;
            if (available && !remoteDesktopView_->isSessionVisible()) {
                openRemoteDesktopViewer(remoteDesktop_->viewerPeerId());
            }
            if (!remoteDesktopView_->isSessionVisible()) return;
            if (!available && remoteDesktopView_->isControlActive(peerUserId)) {
                toggleRemoteDesktopControl(peerUserId);
            }
            if (available) {
                remoteDesktop_->bindRemoteView(
                    peerUserId, remoteDesktopView_->renderWindowHandle(peerUserId));
            }
            remoteDesktopView_->setStreamActive(peerUserId, available);
        });
    // TRTC 接收端实际编码帧分辨率，不等于被控屏幕。它与 Accept 里的
    // CaptureGeometry 一起参与两级换算；此前写死 1920x1080 会在首帧前映射错。
    remoteDesktop_->setRemoteVideoSizeHandler(
        [this](const QString&, int width, int height) {
            const QSize size(width, height);
            if (!size.isValid() || size.isEmpty() || remoteDesktopRemoteVideoSize_ == size) return;
            remoteDesktopRemoteVideoSize_ = size;
            // 控制进行中也要立刻跟上：尺寸是首帧之后才到的，晚一步就意味着
            // 开控制的头几秒鼠标是偏的。
            if (remoteInputCapture_) remoteInputCapture_->setRemoteVideoSize(size);
        });
    // 被控端的状态播报：让用户能区分"对方在等系统授权"和"断网/崩溃"，
    // 而不是对着一块卡住的画面猜。
    connect(remoteDesktop_, &RemoteDesktopController::peerNoticeReceived, this,
            [this](const QString& noticeCode) {
                if (!remoteDesktopView_) return;
                QString text;
                if (noticeCode
                    == QLatin1String(RemoteDesktopSignals::NoticeCodes::kSecureDesktopEntered)) {
                    text = QStringLiteral(
                        "对方电脑弹出了系统授权框（UAC）或已锁屏。这段时间画面会卡住、"
                        "鼠标键盘也点不动，需要有人在那台电脑前操作一下。");
                }
                // 离开安全桌面时 text 为空，正好撤下提示。
                remoteDesktopView_->setNoticeText(QString(), text);
            });

    remoteDesktop_->setErrorHandler([this](int code, const QString& message) {
        if (remoteDesktopView_) {
            for (const QString& peerId : remoteDesktopView_->sessionPeerIds()) {
                if (remoteDesktopView_->isControlActive(peerId)) {
                    toggleRemoteDesktopControl(peerId);
                }
                remoteDesktopView_->setStreamActive(peerId, false);
            }
            remoteDesktopView_->setStatusText(
                QStringLiteral("连接异常（%1）：%2").arg(code).arg(message));
        }
    });
}

void MainWindow::promptRemoteDesktopPassword(const QString& peerUserId) {
    if (peerUserId.isEmpty()) return;
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("需要访问密码");
    options.description =
        QStringLiteral("%1 为远程桌面设置了访问密码，输入后即可连接。").arg(peerUserId);
    options.placeholder = QStringLiteral("对方设置的访问密码");
    options.password = true;
    options.confirmText = QStringLiteral("连接");
    bool ok = false;
    const QString password = AppTextInputDialog::getText(this, options, &ok);
    if (!ok || password.isEmpty()) return;

    // 记住本次会话内的密码，避免重试时反复询问。
    remoteDesktopPasswords_.insert(peerUserId, password);
    remoteDesktop_->requestView(peerUserId, password);
}

void MainWindow::openRemoteDesktopViewer(const QString& peerUserId) {
    if (!remoteDesktopView_) return;
    remoteDesktopView_->beginSession(peerUserId);
    // 自动切到远程页：用户刚点了发起，画面理应立刻可见，不必再手动找。
    showRemotePage();
}

void MainWindow::closeRemoteDesktopViewer() {
    if (!remoteDesktopView_) return;
    remoteDesktopView_->showIdle();
}

void MainWindow::stopRemoteDesktopForShutdown(std::function<void()> completion) {
    if (remoteDesktopShutdown_) {
        if (remoteDesktopShutdownComplete_ && completion) completion();
        return;
    }
    remoteDesktopShutdown_ = true;

    if (remoteInputCapture_) {
        remoteInputCapture_->setEnabled(false);
        remoteInputCapture_->attachTo(nullptr);
    }

    auto completed = std::make_shared<bool>(false);
    auto completeOnce =
        [this, completed, completion = std::move(completion)]() mutable {
            if (*completed) return;
            *completed = true;
            remoteDesktopShutdownComplete_ = true;
            if (completion) completion();
        };

    // SDK 异常时不能让应用永久关不掉；正常情况下 sendText 的回执会更早到。
    QTimer::singleShot(RemoteDesktopStopSendTimeoutMs, this, completeOnce);
    if (remoteDesktop_) {
        remoteDesktop_->stopSession(completeOnce);
    } else {
        completeOnce();
    }
}

void MainWindow::toggleRemoteDesktopControl(const QString& peerUserId) {
    if (!remoteDesktop_ || !remoteDesktopView_) return;
    auto* card = remoteDesktopView_->cardFor(peerUserId);
    if (card == nullptr) return;

    const bool turningOn = !card->isControlActive();
    if (turningOn && !card->isStreamActive()) return;
    if (turningOn) {
        if (!remoteInputCapture_) {
            remoteInputCapture_ =
                std::make_unique<RemoteInputCapture>(remoteDesktop_->inputSender(), this);
            // 急停：鼠标被注入动作带偏时，键盘是唯一还点得中的退路。
            connect(remoteInputCapture_.get(), &RemoteInputCapture::releaseControlRequested, this,
                    [this] {
                        if (!remoteDesktopView_) return;
                        for (const QString& peerId : remoteDesktopView_->sessionPeerIds()) {
                            if (remoteDesktopView_->isControlActive(peerId)) {
                                toggleRemoteDesktopControl(peerId);
                            }
                        }
                    });
        }
        // 一次只控一台：换目标前先把上一台的采集停掉并让它全部放开。
        for (const QString& peerId : remoteDesktopView_->sessionPeerIds()) {
            if (peerId != peerUserId) remoteDesktopView_->setControlActive(peerId, false);
        }
        remoteInputCapture_->attachTo(card->renderSurface());
        // 首帧/尺寸回调前这里是空尺寸，capture 会保持空映射且不发送坐标。
        remoteInputCapture_->setRemoteVideoSize(remoteDesktopRemoteVideoSize_);
        remoteInputCapture_->setEnabled(true);
    } else if (remoteInputCapture_) {
        // 关掉时 capture 会自动发一次"全部抬起"，不留悬空按键。
        remoteInputCapture_->setEnabled(false);
        remoteInputCapture_->attachTo(nullptr);
    }
    remoteDesktopView_->setControlActive(peerUserId, turningOn);
}

void MainWindow::applyRemoteDesktopFullScreen(bool fullScreen) {
    // 只收窗口外壳，不动卡片自身层级：画面渲染在原生子窗口上，
    // 一旦重新 parent 就可能重建 HWND，TRTC 手里的句柄失效直接黑屏。
    if (fullScreen) {
        remoteFullScreenWasMaximized_ = isMaximized();
        showRemotePage();
        if (navRail_) navRail_->hide();
        showFullScreen();
    } else {
        if (navRail_) navRail_->show();
        if (remoteFullScreenWasMaximized_) {
            showMaximized();
        } else {
            showNormal();
        }
    }
}

void MainWindow::handleRemoteDesktopConsent(const QString& fromUserId) {
    RemoteDesktopConsentDialog dialog(fromUserId, RemoteDesktop::kConsentTimeoutMs, this);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    remoteDesktop_->resolveConsent(accepted);
}

void MainWindow::requestRemoteDesktop() {
    if (!remoteDesktop_) return;

    // 会话进行中时同一个按钮就是断开入口（连接中点击则取消）。
    if (remoteDesktop_->viewerState() != RemoteDesktop::ViewerState::Idle
        && remoteDesktop_->viewerState() != RemoteDesktop::ViewerState::Failed) {
        remoteDesktop_->stopSession();
        return;
    }

    const QString peerId = app_.chatState().selectedPeerId();
    if (peerId.isEmpty()) return;

    // 直接发起，不打断用户。绝大多数情况对方靠白名单授权即可；
    // 只有对方额外设了访问密码时才会被拒，那时再按需索取密码。
    remoteDesktop_->requestView(peerId, remoteDesktopPasswords_.value(peerId));
}

void MainWindow::showConversationContextMenu(const QPoint& pos) {
    if (!conversationList_) return;
    QListWidgetItem* item = conversationList_->itemAt(pos);
    if (!item) return;
    conversationList_->setCurrentItem(item);

    QMenu menu(this);
    applyMessageContextMenuStyle(menu);
    QAction* clearAction = menu.addAction(makeLineIcon(LineIconKind::Trash, kMenuIconColor),
                                          QStringLiteral("删除消息"));
    QAction* selectedAction = menu.exec(conversationList_->viewport()->mapToGlobal(pos));
    if (selectedAction == clearAction) {
        clearMessagesFromItem(item);
    }
}

void MainWindow::showContactContextMenu(QListWidget* list, const QPoint& pos) {
    if (!list) return;
    QListWidgetItem* item = list->itemAt(pos);
    if (!item) return;
    list->setCurrentItem(item);

    QMenu menu(this);
    applyMessageContextMenuStyle(menu);
    QAction* deleteAction = menu.addAction(makeLineIcon(LineIconKind::Trash, kMenuIconColor),
                                           QStringLiteral("删除好友"));
    QAction* selectedAction = menu.exec(list->viewport()->mapToGlobal(pos));
    if (selectedAction == deleteAction) {
        deleteContactFromItem(item);
    }
}

void MainWindow::clearMessagesFromItem(QListWidgetItem* item) {
    if (!item) return;
    const QString userId = item->data(UserIdRole).toString().trimmed();
    if (userId.isEmpty()) return;
    const QString displayName = item->data(DisplayNameRole).toString().trimmed();
    if (!AppMessageDialog::confirm(
            this, QStringLiteral("删除消息"),
            QStringLiteral("确定删除与“%1”的全部聊天记录吗？好友会保留，此操作不可恢复。")
                .arg(displayName.isEmpty() ? userId : displayName),
            QStringLiteral("删除"), /*destructive=*/true)) {
        return;
    }
    app_.clearMessagesWith(userId);
}

void MainWindow::deleteContactFromItem(QListWidgetItem* item) {
    if (!item) return;
    const QString userId = item->data(UserIdRole).toString().trimmed();
    if (userId.isEmpty()) return;
    const QString displayName = item->data(DisplayNameRole).toString().trimmed();
    if (!AppMessageDialog::confirm(
            this, QStringLiteral("删除好友"),
            QStringLiteral("确定删除好友“%1”及全部聊天历史吗？此操作不可恢复。")
                .arg(displayName.isEmpty() ? userId : displayName),
            QStringLiteral("删除"), /*destructive=*/true)) {
        return;
    }
    app_.deleteContact(userId);
}

void MainWindow::deleteSelectedContactFromList(QListWidget* list) {
    if (!list) return;
    // 与右键菜单语义一致：会话列表 Delete 只清空聊天记录，删除好友走通讯录。
    if (list == conversationList_) {
        clearMessagesFromItem(list->currentItem());
        return;
    }
    deleteContactFromItem(list->currentItem());
}

QString MainWindow::contactName(const QString& userId) const {
    for (const RemoteIMContact& contact : app_.chatState().contacts()) {
        if (contact.userId == userId) return contact.displayName.isEmpty() ? contact.userId : contact.displayName;
    }
    return userId;
}
