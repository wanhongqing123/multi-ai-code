#include "markdown/MarkdownView.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <memory>
#include <vector>

#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayout.h"

// 一条内容。位置都在**内容坐标系**里：y 从整片的顶上算起，和滚动位置无关。
struct MarkdownViewItem {
    QString id;
    MarkdownView::Style style = MarkdownView::Style::Document;
    QString source;
    MarkdownDocument document;
    MarkdownLayout layout;

    // 外框（气泡的底、整行的范围）和里面文字的左上角。
    QRectF frame;
    QPointF origin;

    // 选区用：这条内容的文字在整片里的起点。
    int textStart = 0;

    // 嵌进来的部件（工具卡那种）。非空时这条不走 Markdown 那套。
    QWidget* widget = nullptr;
    // 上次给它留的高度。部件自己变高时要认出来，但 setGeometry 又会回弹
    // 一个 Resize 事件——比高度才能分清「真变了」和「我自己刚摆的」。
    int widgetHeight = 0;

    int textLength() const {
        return widget != nullptr ? 0 : layout.textLength();
    }
};

namespace {

using Item = MarkdownViewItem;

// 给嵌进来的部件留多高。
//
// **不能只看 sizeHint。** QLabel 之类的 sizeHint 给的是文字的自然高度，
// 调用方 setFixedHeight 之后它一点不变——工具卡展开了这儿还按旧高度留位，
// 卡片就会盖住下一条消息。这里按 Qt 布局的规矩来：先问 heightForWidth，
// 没有就用 sizeHint，最后夹到 [minimumHeight, maximumHeight] 之间。
// 给它留多宽。不想撑满的部件（工具卡：横向策略 Fixed / Maximum）按自己的
// sizeHint 走，靠左摆——撑满会让一条注记看起来比回答还重要。
int widgetSlotWidth(const QWidget* widget, int available) {
    const QSizePolicy::Policy policy = widget->sizePolicy().horizontalPolicy();
    if (policy != QSizePolicy::Fixed && policy != QSizePolicy::Maximum) return available;
    return qBound(1, widget->sizeHint().width(), available);
}

int widgetSlotHeight(const QWidget* widget, int width) {
    int height = widget->sizeHint().height();
    if (widget->hasHeightForWidth() && width > 0) {
        height = qMax(height, widget->heightForWidth(width));
    }
    return qBound(widget->minimumHeight(), qMax(height, 1), widget->maximumHeight());
}

}  // namespace

struct MarkdownView::Runtime {
    MarkdownTheme theme = MarkdownTheme::standard(1.0);
    std::vector<std::unique_ptr<Item>> items;
    qreal contentHeight = 0;
    int maxContentWidth = 0;

    // 贴底跟随。用户自己往上滚过就停掉——正在看历史却被拽回底部很烦。
    bool stickToBottom = true;

    // 选区，整片的字符偏移。
    int anchor = 0;
    int cursor = 0;
    bool selecting = false;
    QPoint pressPosition;

    Item* find(const QString& id) const {
        for (const std::unique_ptr<Item>& item : items) {
            if (item->id == id) return item.get();
        }
        return nullptr;
    }
};

MarkdownView::MarkdownView(QWidget* parent)
    : QAbstractScrollArea(parent), runtime_(std::make_unique<Runtime>()) {
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(false);
    // 文字上要显示 I 形光标，链接上要显示手形——两种都在 mouseMoveEvent 里切。
    viewport()->setCursor(Qt::IBeamCursor);
    setFocusPolicy(Qt::StrongFocus);

    applyScrollBarStyle();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        runtime_->stickToBottom = value >= verticalScrollBar()->maximum() - 2;
    });
}

MarkdownView::~MarkdownView() = default;

void MarkdownView::setTheme(const MarkdownTheme& theme) {
    runtime_->theme = theme;
    applyScrollBarStyle();
    // 主题换了字号和间距都会变，整片重排。
    for (const std::unique_ptr<Item>& item : runtime_->items) item->layout = MarkdownLayout();
    relayoutAll();
}

// 细滚动条，和 MaiChat 会话列表那两条一致。
//
// 样式表**只挂在自己的滚动条上**，不往上挂：挂在视图上的话会顺着层级
// 落到嵌进来的工具卡里去。上下箭头按钮要置零，8px 宽度下它们会挤成两个色块——
// 原生那两个箭头正是这么来的。
void MarkdownView::applyScrollBarStyle() {
    const MarkdownTheme& theme = runtime_->theme;
    verticalScrollBar()->setStyleSheet(
        QStringLiteral("QScrollBar:vertical{background:transparent;width:%1px;margin:0;}"
                       "QScrollBar::handle:vertical{background:%2;border-radius:%3px;"
                       "min-height:%4px;}"
                       "QScrollBar::handle:vertical:hover{background:%5;}"
                       "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{"
                       "height:0;border:0;background:transparent;}"
                       "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{"
                       "background:transparent;}")
            .arg(theme.scrollBarWidth)
            .arg(theme.scrollBarHandle.name())
            .arg(theme.scrollBarWidth / 2)
            .arg(theme.scrollBarWidth * 3)
            .arg(theme.scrollBarHandleHover.name()));
}

const MarkdownTheme& MarkdownView::theme() const {
    return runtime_->theme;
}

void MarkdownView::setMaxContentWidth(int width) {
    if (runtime_->maxContentWidth == width) return;
    runtime_->maxContentWidth = qMax(0, width);
    relayoutAll();
}

void MarkdownView::addItem(const QString& id, Style style, const QString& markdown) {
    if (Item* existing = runtime_->find(id)) {
        existing->style = style;
        existing->source = markdown;
        existing->document = MarkdownDocument::parse(markdown);
        relayoutAll();
        return;
    }
    auto item = std::make_unique<Item>();
    item->id = id;
    item->style = style;
    item->source = markdown;
    item->document = MarkdownDocument::parse(markdown);
    runtime_->items.push_back(std::move(item));
    relayoutAll();
}

void MarkdownView::updateItem(const QString& id, const QString& markdown) {
    Item* item = runtime_->find(id);
    if (item == nullptr) return;
    if (item->source == markdown) return;
    item->source = markdown;
    item->document = MarkdownDocument::parse(markdown);
    relayoutAll();
}

bool MarkdownView::contains(const QString& id) const {
    return runtime_->find(id) != nullptr;
}

void MarkdownView::removeItem(const QString& id) {
    for (auto it = runtime_->items.begin(); it != runtime_->items.end(); ++it) {
        if ((*it)->id != id) continue;
        if ((*it)->widget != nullptr) (*it)->widget->deleteLater();
        runtime_->items.erase(it);
        relayoutAll();
        return;
    }
}

void MarkdownView::addWidget(const QString& id, QWidget* widget) {
    if (widget == nullptr) return;
    widget->setParent(viewport());
    widget->installEventFilter(this);
    auto item = std::make_unique<Item>();
    item->id = id;
    item->widget = widget;
    runtime_->items.push_back(std::move(item));
    relayoutAll();
}

void MarkdownView::clear() {
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget == nullptr) continue;
        // deleteLater 要等下一轮事件循环才真正销毁；不先 hide 的话，这段窗口期里
        // 旧部件还盖在新画的内容上（切会话后立刻看到上一会话的残影/遮挡），
        // 而且点击会落在这些死部件上，怎么点都没反应。hide 是同步的，立刻出视野。
        item->widget->hide();
        item->widget->deleteLater();
    }
    runtime_->items.clear();
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    runtime_->stickToBottom = true;
    relayoutAll();
}

bool MarkdownView::isEmpty() const {
    return runtime_->items.empty();
}

int MarkdownView::itemCount() const {
    return static_cast<int>(runtime_->items.size());
}

QRectF MarkdownView::itemRect(const QString& id) const {
    const Item* item = runtime_->find(id);
    return item == nullptr ? QRectF() : item->frame;
}

qreal MarkdownView::contentHeight() const {
    return runtime_->contentHeight;
}

void MarkdownView::scrollToBottom() {
    runtime_->stickToBottom = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

bool MarkdownView::isAtBottom() const {
    return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 2;
}

void MarkdownView::relayoutAll() {
    const MarkdownTheme& theme = runtime_->theme;
    const qreal margin = theme.viewMargin;
    const qreal full = qMax<qreal>(viewport()->width() - 2 * margin, 1);
    const qreal available =
        runtime_->maxContentWidth > 0 ? qMin<qreal>(full, runtime_->maxContentWidth) : full;
    // 列宽不够占满时居中。原来是靠 addStretch(1)/addWidget(列,20)/addStretch(1)
    // 那种权重摆出来的，权重给错一次列就只剩三分之一宽。
    const qreal left = margin + (full - available) / 2;

    qreal y = margin;
    int textStart = 0;
    bool first = true;
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (!first) y += theme.itemSpacing;
        first = false;
        item->textStart = textStart;

        if (item->widget != nullptr) {
            const int slotWidth = widgetSlotWidth(item->widget, qRound(available));
            const int height = widgetSlotHeight(item->widget, slotWidth);
            item->widgetHeight = height;
            item->frame = QRectF(left, y, slotWidth, height);
            item->origin = item->frame.topLeft();
            y = item->frame.bottom();
            continue;
        }

        switch (item->style) {
            case Style::Document: {
                item->layout.layout(item->document, theme, available);
                item->frame = QRectF(left, y, available, item->layout.height());
                item->origin = item->frame.topLeft();
                break;
            }
            case Style::Bubble: {
                // 先按最大宽度排一遍，再按实际需要的宽度收窄。短消息不该占满一行，
                // 而**宽度只能量出来不能估**——估窄了字会被提前折断。
                const qreal maximum =
                    qMax<qreal>(available * theme.bubbleMaxWidthRatio - 2 * theme.bubblePadding, 1);
                item->layout.layout(item->document, theme, maximum);
                const qreal natural = qMin(item->layout.naturalWidth(), maximum);
                if (natural > 0 && natural < maximum) {
                    item->layout.layout(item->document, theme, natural);
                }
                const qreal width = item->layout.width() + 2 * theme.bubblePadding;
                const qreal height = item->layout.height() + 2 * theme.bubblePadding;
                item->frame = QRectF(left + available - width, y, width, height);
                item->origin =
                    item->frame.topLeft() + QPointF(theme.bubblePadding, theme.bubblePadding);
                break;
            }
            case Style::Notice:
            case Style::Error: {
                // 提示行要用自己的颜色，不能和正文一个色——它不是内容，
                // 是「刚才发生了什么」，读的时候应该一眼掠过去。
                MarkdownTheme muted = theme;
                muted.text = item->style == Style::Error ? theme.errorText : theme.noticeText;
                muted.bodyPixelSize = qMax(1, theme.bodyPixelSize - 2);
                item->layout.layout(item->document, muted, available);
                const qreal width = qMin(item->layout.naturalWidth(), available);
                item->layout.layout(item->document, muted, qMax<qreal>(width, 1));
                item->frame =
                    QRectF(left + (available - item->layout.width()) / 2, y,
                           item->layout.width(), item->layout.height());
                item->origin = item->frame.topLeft();
                break;
            }
        }
        y = item->frame.bottom();
        textStart += item->textLength();
    }

    runtime_->contentHeight = y + margin;
    updateScrollRange();
    applySelectionToItems();
    positionWidgets();
    viewport()->update();
}

void MarkdownView::updateScrollRange() {
    const int overflow =
        qMax(0, qRound(runtime_->contentHeight) - viewport()->height());
    QScrollBar* bar = verticalScrollBar();
    const bool wasAtBottom = runtime_->stickToBottom;
    bar->setRange(0, overflow);
    bar->setPageStep(viewport()->height());
    bar->setSingleStep(qMax(1, runtime_->theme.bodyPixelSize * 3));
    if (wasAtBottom) bar->setValue(overflow);
}

void MarkdownView::positionWidgets() {
    const int offset = verticalScrollBar()->value();
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget == nullptr) continue;
        const QRectF frame = item->frame.translated(0, -offset);
        // 滚出可见区就藏起来：Qt 不会自己裁部件，留着会盖在别的东西上面。
        const bool visible = frame.bottom() >= 0 && frame.top() <= viewport()->height();
        item->widget->setVisible(visible);
        if (!visible) continue;
        item->widget->setGeometry(frame.toRect());
    }
}

void MarkdownView::paintEvent(QPaintEvent* event) {
    const MarkdownTheme& theme = runtime_->theme;
    QPainter painter(viewport());
    painter.fillRect(event->rect(), theme.viewBackground);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int offset = verticalScrollBar()->value();
    const QRectF visible(0, 0, viewport()->width(), viewport()->height());

    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const QRectF frame = item->frame.translated(0, -offset);
        if (!visible.intersects(frame)) continue;

        if (item->style == Style::Bubble) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.bubbleBackground);
            painter.drawRoundedRect(frame, theme.bubbleRadius, theme.bubbleRadius);
        }
        item->layout.paint(&painter, item->origin - QPointF(0, offset), visible);
    }
}

void MarkdownView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    relayoutAll();
}

bool MarkdownView::eventFilter(QObject* watched, QEvent* event) {
    // 工具卡展开、思考条收起之后高度变了，后面所有东西都要往下挪。
    // 只认高度真的变了的情况：setGeometry 自己也会回弹一个 Resize，
    // 不比高度的话这里会无限递归。
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize) {
        for (const std::unique_ptr<Item>& item : runtime_->items) {
            if (item->widget != watched) continue;
            const qreal available = item->frame.width();
            if (widgetSlotHeight(item->widget, qRound(available)) == item->widgetHeight) break;
            relayoutAll();
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void MarkdownView::scrollContentsBy(int dx, int dy) {
    QAbstractScrollArea::scrollContentsBy(dx, dy);
    positionWidgets();
    viewport()->update();
}

// ── 选区与命中 ──────────────────────────────────────────────────

const MarkdownViewItem* MarkdownView::itemAt(const QPoint& viewportPoint, QPointF* local) const {
    const int offset = verticalScrollBar()->value();
    const QPointF content(viewportPoint.x(), viewportPoint.y() + offset);

    const Item* best = nullptr;
    qreal bestDistance = 0;
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const qreal top = item->frame.top();
        const qreal bottom = item->frame.bottom();
        if (content.y() >= top && content.y() <= bottom) {
            best = item.get();
            break;
        }
        // 拖到两条消息之间的空档时要挑最近的，不然选区会在那儿断掉。
        const qreal distance = content.y() < top ? top - content.y() : content.y() - bottom;
        if (best == nullptr || distance < bestDistance) {
            best = item.get();
            bestDistance = distance;
        }
    }
    if (best != nullptr && local != nullptr) *local = content - best->origin;
    return best;
}

int MarkdownView::positionAt(const QPoint& viewportPoint) const {
    QPointF local;
    const Item* item = itemAt(viewportPoint, &local);
    if (item == nullptr) return 0;
    return item->textStart + item->layout.positionAt(local);
}

void MarkdownView::applySelectionToItems() {
    const int from = qMin(runtime_->anchor, runtime_->cursor);
    const int to = qMax(runtime_->anchor, runtime_->cursor);
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const int start = item->textStart;
        const int end = start + item->textLength();
        if (to <= start || from >= end) {
            item->layout.clearSelection();
            continue;
        }
        item->layout.setSelection(qMax(from, start) - start, qMin(to, end) - start);
    }
}

void MarkdownView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    runtime_->pressPosition = event->pos();
    runtime_->anchor = positionAt(event->pos());
    runtime_->cursor = runtime_->anchor;
    runtime_->selecting = true;
    applySelectionToItems();
    viewport()->update();
}

void MarkdownView::mouseMoveEvent(QMouseEvent* event) {
    if (runtime_->selecting) {
        runtime_->cursor = positionAt(event->pos());
        applySelectionToItems();
        viewport()->update();
        return;
    }
    QPointF local;
    const Item* item = itemAt(event->pos(), &local);
    const bool onLink = item != nullptr && !item->layout.linkAt(local).isEmpty();
    viewport()->setCursor(onLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void MarkdownView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    runtime_->selecting = false;
    // 拖过一段距离的是在选文字，不是在点链接。
    const int moved = (event->pos() - runtime_->pressPosition).manhattanLength();
    if (moved > QApplication::startDragDistance()) return;

    QPointF local;
    const Item* item = itemAt(event->pos(), &local);
    if (item == nullptr) return;
    const QString href = item->layout.linkAt(local);
    if (!href.isEmpty()) emit linkActivated(href);
}

void MarkdownView::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void MarkdownView::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* copy = menu.addAction(QStringLiteral("复制"));
    copy->setEnabled(!selectedText().isEmpty());
    QAction* all = menu.addAction(QStringLiteral("全选"));

    QPointF local;
    const Item* item = itemAt(event->pos(), &local);
    QAction* copySource = nullptr;
    if (item != nullptr && !item->source.isEmpty()) {
        // 渲染是有损的：标题、代码围栏、链接目标都还原不回来，
        // 所以「复制原始数据」复制的是 Markdown 原文。IM 那边一直有这一条。
        copySource = menu.addAction(QStringLiteral("复制原始数据"));
    }

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == nullptr) return;
    if (chosen == copy) {
        copySelection();
    } else if (chosen == all) {
        selectAll();
    } else if (chosen == copySource) {
        QApplication::clipboard()->setText(item->source);
    }
}

QString MarkdownView::selectedText() const {
    QString text;
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const QString piece = item->layout.selectedText();
        if (piece.isEmpty()) continue;
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) text += QLatin1Char('\n');
        text += piece;
    }
    return text;
}

void MarkdownView::selectAll() {
    int total = 0;
    for (const std::unique_ptr<Item>& item : runtime_->items) total += item->textLength();
    runtime_->anchor = 0;
    runtime_->cursor = total;
    applySelectionToItems();
    viewport()->update();
}

void MarkdownView::clearSelection() {
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    applySelectionToItems();
    viewport()->update();
}

void MarkdownView::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) return;
    QApplication::clipboard()->setText(text);
}
