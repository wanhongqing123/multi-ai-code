#include "markdown/MarkdownLabel.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayout.h"
#include "markdown/MarkdownLayoutCache.h"

struct MarkdownLabel::Runtime {
    MarkdownTheme theme = MarkdownTheme::standard(1.0);
    MarkdownDocument document;

    QString noteText;
    QColor noteColor;
    int notePixelSize = 0;

    // ── 为什么有两份版 ──────────────────────────────────────────
    //
    // 排版是这里最贵的一步，而它有**两个互相打架的调用方**：
    //
    //   布局    heightForWidth(w)：定案之前会拿好几个试探宽度来问高度
    //   绘制    永远要按部件自己那个宽度
    //
    // 只留一份的话，两边会把对方的结果反复踢掉——布局问一次重排一次，
    // 画一次又重排回来。一屏几十条消息，切个会话就是几百次整段重排。
    //
    // 所以量归量、画归画。量完的那份如果正好是部件要的宽度，直接搬过来用
    // （MarkdownLayout 是可移动的），常见路径下一条消息只排一次版。
    MarkdownLayout layout;   // 画用
    qreal laidOutWidth = -1;
    MarkdownLayout probe;    // 量用
    qreal probedWidth = -1;
    // 画用的这份是不是从缓存里取来的（或者已经可以还回去了）。
    // 只有真排过版的才值得还。
    bool layoutWorthCaching = false;

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

MarkdownLabel::~MarkdownLabel() {
    // 部件没了，排好的版还能用：切回这个会话时内容和宽度都还是这一套。
    returnLayoutToCache();
}

void MarkdownLabel::setTheme(const MarkdownTheme& theme) {
    runtime_->theme = theme;
    invalidate();
}

const MarkdownTheme& MarkdownLabel::theme() const {
    return runtime_->theme;
}

void MarkdownLabel::setMarkdown(const QString& markdown) {
    runtime_->document = MarkdownDocument::parse(markdown);
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    invalidate();
}

const QString& MarkdownLabel::markdown() const {
    // 块树自己留着原文，不用再存一份。
    return runtime_->document.source();
}

void MarkdownLabel::setTrailingNote(const QString& text, const QColor& color, int pixelSize) {
    runtime_->noteText = text;
    runtime_->noteColor = color;
    runtime_->notePixelSize = pixelSize;
    invalidate();
}

// 内容或皮肤变了：**只作废，不排版**。
//
// 原来这三个 setter 每个都立刻排一次版，而那时候部件往往还是默认的 100 宽——
// 一条消息光是建起来就要排四次，三次是白排的（两次在错的宽度上，一次被下一次
// 覆盖）。现在等到真要用的时候（量高度或者绘制）再排，那时宽度也定了。
void MarkdownLabel::invalidate() {
    returnLayoutToCache();
    runtime_->laidOutWidth = -1;
    runtime_->probedWidth = -1;
    updateGeometry();
    update();
}

// 把排好的版交还给缓存。内容或宽度要变之前、以及部件析构时调用。
//
// **量用的那份也要还。** 一条消息刚建出来时还没拿到真实宽度（QWidget 的默认
// 宽度是 100），布局会先按那个宽度问一次高度；那次的结果如果不留着，每次切
// 会话都要白排一遍——实测就是这一笔占掉了大半开销。
void MarkdownLabel::returnLayoutToCache() {
    Runtime& runtime = *runtime_;
    const auto give = [&runtime](MarkdownLayout& layout, qreal& width) {
        if (width < 0) return;
        auto owned = std::make_unique<MarkdownLayout>(std::move(layout));
        layout = MarkdownLayout();
        MarkdownLayoutCache::put(runtime.document.source(), runtime.noteText,
                                 runtime.notePixelSize, runtime.noteColor.rgba(), width,
                                 runtime.theme.zoom, std::move(owned));
        width = -1;
    };
    if (runtime.layoutWorthCaching) give(runtime.layout, runtime.laidOutWidth);
    runtime.layoutWorthCaching = false;
    give(runtime.probe, runtime.probedWidth);
}

// 只把量用的那份还回去。换宽度量之前用。
void MarkdownLabel::returnProbeToCache() {
    Runtime& runtime = *runtime_;
    if (runtime.probedWidth < 0) return;
    auto owned = std::make_unique<MarkdownLayout>(std::move(runtime.probe));
    runtime.probe = MarkdownLayout();
    MarkdownLayoutCache::put(runtime.document.source(), runtime.noteText, runtime.notePixelSize,
                             runtime.noteColor.rgba(), runtime.probedWidth, runtime.theme.zoom,
                             std::move(owned));
    runtime.probedWidth = -1;
}

QString MarkdownLabel::plainText() const {
    ensureCurrentLayout();
    QString text = runtime_->layout.selectableText();
    // 排版层在每个段落后面补了换行，方便复制时保持分段；对外报「画了什么字」
    // 时把尾巴上那个去掉，不然任何 endsWith 都要先猜有没有换行。
    while (text.endsWith(QLatin1Char('\n'))) text.chop(1);
    return text;
}

int MarkdownLabel::contentHeight() const {
    // 「当前宽度下的内容高度」——量高度可能把版排到别的宽度上去了，先纠回来。
    ensureCurrentLayout();
    return qCeil(runtime_->layout.height());
}

bool MarkdownLabel::hasHeightForWidth() const {
    return true;
}

int MarkdownLabel::heightForWidth(int width) const {
    const qreal target = qMax(width, kMinimumLayoutWidth);
    Runtime& runtime = *runtime_;
    // 画用的那份正好是这个宽度，直接拿。
    if (qFuzzyCompare(runtime.laidOutWidth, target)) return qCeil(runtime.layout.height());
    // 上次量的还是这个宽度，也直接拿。Qt 的布局会为同一个宽度反复问。
    if (qFuzzyCompare(runtime.probedWidth, target)) return qCeil(runtime.probe.height());

    // 要换一个宽度量了，手上这份先还回去。
    //
    // **这一步漏了就等于没有缓存。** Qt 的布局对每条消息至少问两个宽度
    // （先按 sizeHint 的宽度，再按真实宽度），直接覆盖的话前一个宽度的结果
    // 每次都被丢掉、每次都要重排——实测一次切换能多出三千次未命中。
    const_cast<MarkdownLabel*>(this)->returnProbeToCache();

    // 缓存里有现成的就直接拿，连量都不用量。
    if (auto cached = MarkdownLayoutCache::take(runtime.document.source(), runtime.noteText,
                                                runtime.notePixelSize, runtime.noteColor.rgba(),
                                                target, runtime.theme.zoom)) {
        runtime.probe = std::move(*cached);
        runtime.probedWidth = target;
        return qCeil(runtime.probe.height());
    }

    // 真量一次。**不碰画用的那份**——碰了就会和绘制来回抢。
    // const 的语义是「对外看不出变化」，排版结果只是缓存。
    runtime.probe.setTrailingNote(runtime.noteText, runtime.noteColor, runtime.notePixelSize);
    runtime.probe.layout(runtime.document, runtime.theme, target);
    runtime.probedWidth = target;
    return qCeil(runtime.probe.height());
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
    ensureCurrentLayout();
    return runtime_->layout.linkAt(QPointF(point));
}

QString MarkdownLabel::selectedText() const {
    ensureCurrentLayout();
    return runtime_->layout.selectedText();
}

bool MarkdownLabel::hasSelection() const {
    return runtime_->anchor != runtime_->cursor;
}

void MarkdownLabel::selectAll() {
    ensureCurrentLayout();
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

// 保证画用的这份版是按 width 排的。
void MarkdownLabel::ensureLayout(qreal width) {
    if (qFuzzyCompare(runtime_->laidOutWidth, width)) return;
    // 要换宽度了，画用的这份先还回去——切回来的时候多半还是那个宽度。
    // 量用的那份不动：下面几行可能正好要用它。
    if (runtime_->layoutWorthCaching && runtime_->laidOutWidth >= 0) {
        auto owned = std::make_unique<MarkdownLayout>(std::move(runtime_->layout));
        runtime_->layout = MarkdownLayout();
        MarkdownLayoutCache::put(runtime_->document.source(), runtime_->noteText,
                                 runtime_->notePixelSize, runtime_->noteColor.rgba(),
                                 runtime_->laidOutWidth, runtime_->theme.zoom, std::move(owned));
        runtime_->laidOutWidth = -1;
    }
    runtime_->layoutWorthCaching = false;

    // 刚才量高度时已经按这个宽度排过了，搬过来就行，别再排一遍。
    // 常见路径就是这条：布局先问 heightForWidth(真实宽度)，随后才绘制。
    if (qFuzzyCompare(runtime_->probedWidth, width)) {
        runtime_->layout = std::move(runtime_->probe);
        runtime_->probe = MarkdownLayout();
        runtime_->probedWidth = -1;
    } else if (auto cached = MarkdownLayoutCache::take(
                   runtime_->document.source(), runtime_->noteText, runtime_->notePixelSize,
                   runtime_->noteColor.rgba(), width, runtime_->theme.zoom)) {
        // 这条消息之前排过，内容宽度皮肤都没变——直接用，省掉最贵的一步。
        runtime_->layout = std::move(*cached);
    } else {
        runtime_->layout.setTrailingNote(runtime_->noteText, runtime_->noteColor,
                                         runtime_->notePixelSize);
        runtime_->layout.layout(runtime_->document, runtime_->theme, width);
    }
    runtime_->layout.setSelection(runtime_->anchor, runtime_->cursor);
    runtime_->laidOutWidth = width;
    runtime_->layoutWorthCaching = true;
}

void MarkdownLabel::paintEvent(QPaintEvent* event) {
    // **画之前先确认这份版是按自己当前宽度排的。**
    //
    // heightForWidth() 会把版排到调用方问的那个宽度上——Qt 的布局在定案之前
    // 会拿好几个试探宽度来问。要是最后一次问的是 120，而部件实际有 600 宽，
    // 这里直接画就会按 120 折行：一句话被切成好几行，右边大片空白。
    // 缓存命中时这一句是免费的。
    ensureLayout(qMax(width(), kMinimumLayoutWidth));

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    runtime_->layout.paint(&painter, QPointF(0, 0), QRectF(event->rect()));
}

void MarkdownLabel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->size().width() == event->oldSize().width()) return;
    // 这里也不急着排——等绘制或者量高度的时候再排，那时才知道是不是真要用。
    updateGeometry();
    update();
}

// 选区和命中测试也都要按当前宽度来：坐标是拿这份版算的。
void MarkdownLabel::ensureCurrentLayout() const {
    const_cast<MarkdownLabel*>(this)->ensureLayout(qMax(width(), kMinimumLayoutWidth));
}

void MarkdownLabel::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    runtime_->pressPosition = event->pos();
    ensureCurrentLayout();
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
