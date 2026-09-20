#include "markdown/MarkdownLabel.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayout.h"

struct MarkdownLabel::Runtime {
    MarkdownTheme theme = MarkdownTheme::standard(1.0);
    MarkdownDocument document;
    MarkdownLayout layout;

    QString noteText;
    QColor noteColor;
    int notePixelSize = 0;

    // 上一次排版用的宽度。宽度没变就别重排——排版是这里最贵的一步，
    // 而 Qt 的布局会为同一个宽度反复问高度。
    qreal laidOutWidth = -1;

    bool selecting = false;
    QPoint pressPosition;
    int anchor = 0;
    int cursor = 0;
};

namespace {

// 排版要的最小宽度。0 宽度下 QTextLayout 会把每个字断成一行，高度飙到天上，
// 而布局在初始化时确实会拿 0 来问一次。
constexpr int kMinimumLayoutWidth = 24;

}  // namespace

MarkdownLabel::MarkdownLabel(QWidget* parent)
    : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::IBeamCursor);
    // 气泡的底色由外面那层画，这里只画字。
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAutoFillBackground(false);
}

MarkdownLabel::~MarkdownLabel() = default;

void MarkdownLabel::setTheme(const MarkdownTheme& theme) {
    runtime_->theme = theme;
    runtime_->laidOutWidth = -1;
    relayout();
    updateGeometry();
    update();
}

const MarkdownTheme& MarkdownLabel::theme() const {
    return runtime_->theme;
}

void MarkdownLabel::setMarkdown(const QString& markdown) {
    runtime_->document = MarkdownDocument::parse(markdown);
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    runtime_->laidOutWidth = -1;
    relayout();
    updateGeometry();
    update();
}

const QString& MarkdownLabel::markdown() const {
    // 块树自己留着原文，不用再存一份。
    return runtime_->document.source();
}

void MarkdownLabel::setTrailingNote(const QString& text, const QColor& color, int pixelSize) {
    runtime_->noteText = text;
    runtime_->noteColor = color;
    runtime_->notePixelSize = pixelSize;
    runtime_->laidOutWidth = -1;
    relayout();
    updateGeometry();
    update();
}

QString MarkdownLabel::plainText() const {
    QString text = runtime_->layout.selectableText();
    // 排版层在每个段落后面补了换行，方便复制时保持分段；对外报「画了什么字」
    // 时把尾巴上那个去掉，不然任何 endsWith 都要先猜有没有换行。
    while (text.endsWith(QLatin1Char('\n'))) text.chop(1);
    return text;
}

int MarkdownLabel::contentHeight() const {
    return qCeil(runtime_->layout.height());
}

bool MarkdownLabel::hasHeightForWidth() const {
    return true;
}

int MarkdownLabel::heightForWidth(int width) const {
    const qreal target = qMax(width, kMinimumLayoutWidth);
    if (!qFuzzyCompare(runtime_->laidOutWidth, target)) {
        // 布局问高度时还没真的 resize，这里得按它问的宽度排一遍。
        // const 的语义是「对外看不出变化」——排版结果只是缓存。
        Runtime& runtime = *runtime_;
        runtime.layout.setTrailingNote(runtime.noteText, runtime.noteColor, runtime.notePixelSize);
        runtime.layout.layout(runtime.document, runtime.theme, target);
        runtime.layout.setSelection(runtime.anchor, runtime.cursor);
        runtime.laidOutWidth = target;
    }
    return qCeil(runtime_->layout.height());
}

QSize MarkdownLabel::sizeHint() const {
    // 宽度这一项沿用原来 QTextBrowser 版本给的 360：气泡是按行布局的拉伸位
    // 摆的，这个值只在「一行里怎么分」时起作用，换成按内容算会改动现有气泡宽度。
    return QSize(360, qMax(contentHeight(), 1));
}

QSize MarkdownLabel::minimumSizeHint() const {
    return QSize(kMinimumLayoutWidth, 1);
}

QString MarkdownLabel::linkAt(const QPoint& point) const {
    return runtime_->layout.linkAt(QPointF(point));
}

QString MarkdownLabel::selectedText() const {
    return runtime_->layout.selectedText();
}

bool MarkdownLabel::hasSelection() const {
    return runtime_->anchor != runtime_->cursor;
}

void MarkdownLabel::selectAll() {
    runtime_->anchor = 0;
    runtime_->cursor = runtime_->layout.textLength();
    runtime_->layout.setSelection(runtime_->anchor, runtime_->cursor);
    update();
}

void MarkdownLabel::clearSelection() {
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    runtime_->layout.clearSelection();
    update();
}

void MarkdownLabel::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) return;
    QApplication::clipboard()->setText(text);
}

void MarkdownLabel::relayout() {
    const qreal width = qMax(this->width(), kMinimumLayoutWidth);
    runtime_->layout.setTrailingNote(runtime_->noteText, runtime_->noteColor,
                                     runtime_->notePixelSize);
    runtime_->layout.layout(runtime_->document, runtime_->theme, width);
    runtime_->layout.setSelection(runtime_->anchor, runtime_->cursor);
    runtime_->laidOutWidth = width;
}

void MarkdownLabel::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    runtime_->layout.paint(&painter, QPointF(0, 0), QRectF(event->rect()));
}

void MarkdownLabel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->size().width() == event->oldSize().width()) return;
    relayout();
    updateGeometry();
}

void MarkdownLabel::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    runtime_->pressPosition = event->pos();
    runtime_->anchor = runtime_->layout.positionAt(QPointF(event->pos()));
    runtime_->cursor = runtime_->anchor;
    runtime_->selecting = true;
    runtime_->layout.setSelection(runtime_->anchor, runtime_->cursor);
    update();
}

void MarkdownLabel::mouseMoveEvent(QMouseEvent* event) {
    if (runtime_->selecting) {
        runtime_->cursor = runtime_->layout.positionAt(QPointF(event->pos()));
        runtime_->layout.setSelection(runtime_->anchor, runtime_->cursor);
        update();
        return;
    }
    setCursor(linkAt(event->pos()).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
}

void MarkdownLabel::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    runtime_->selecting = false;
    // 拖过一段距离的是在选字，不是在点链接。
    if ((event->pos() - runtime_->pressPosition).manhattanLength()
        > QApplication::startDragDistance()) {
        return;
    }
    const QString href = linkAt(event->pos());
    if (!href.isEmpty()) emit linkActivated(href);
}

void MarkdownLabel::keyPressEvent(QKeyEvent* event) {
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
    QWidget::keyPressEvent(event);
}
