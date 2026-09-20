#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <memory>

#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownTheme.h"

class QPainter;

// 把块树排到像素上：**一次排版，多次绘制**。
//
// ── 为什么要自己排，而不是接着用 QTextDocument ──────────────────
//
// QTextDocument 认的是 CSS 的一个子集，行内 padding、块级圆角、border-left、
// 复选框全都不支持。原来的 MarkdownRenderer 只能拿表格去模拟引用块和代码块，
// 再往上走（代码块圆角、行内代码的胶囊底、任务列表的真复选框、语法高亮、
// 长输出折叠）都做不到。这里用 QTextLayout 逐段排版、自己画装饰，那些限制就没了。
//
// ── 为什么不是一条消息一个 QLabel ───────────────────────────────
//
// 部件是有成本的：每个 QLabel 都是一个窗口级对象，几百条消息下来创建、布局、
// 样式表匹配的开销很可观，滚动时还会整列重算。这里一篇内容只产出一批
// QTextLayout 和几个矩形，绘制时按可见区域裁剪——**屏幕外的东西一笔都不画**。
// iOS 那边是一样的路子：解析交给库，绘制自己在 view 上做。
//
// ── 怎么用 ────────────────────────────────────────────────────
//
//   layout(document, theme, width);   // 宽度没变就别重复调，这一步是贵的
//   painter->...; paint(painter, origin, clip);
//
// 线程：只在界面线程用。QTextLayout 会碰字体引擎。
class MarkdownLayout {
public:
    MarkdownLayout();
    ~MarkdownLayout();
    MarkdownLayout(MarkdownLayout&&) noexcept;
    MarkdownLayout& operator=(MarkdownLayout&&) noexcept;
    MarkdownLayout(const MarkdownLayout&) = delete;
    MarkdownLayout& operator=(const MarkdownLayout&) = delete;

    // 正文末尾那一小段附注。IM 的消息时间戳用它。
    //
    // **接在最后一行的末尾，不另起一行。** 另起一行的话，一条「好」这样的单行消息
    // 会凭空多出一整行高，气泡看起来空荡荡的——原来那版用 QTextCursor 往文档尾巴上
    // 插一段字来做这件事，换成自绘之后没有文档可插了，所以做成排版层的一个入参。
    //
    // 要在 layout() **之前**设好：它参与排版，不是画完再贴上去的。
    // 附注算进可选文字，和原来 QTextBrowser 的行为一致（复制会带上时间）。
    void setTrailingNote(const QString& text, const QColor& color, int pixelSize);

    // 按给定宽度排版。width 是**内容宽度**，不含调用方自己的外边距。
    void layout(const MarkdownDocument& document, const MarkdownTheme& theme, qreal width);

    qreal width() const;
    qreal height() const;
    // 不折行时这篇内容需要多宽。气泡要「短消息就窄一点」，得先知道这个。
    // **不要用 QFontMetrics 去估**——上一版气泡就是估窄了，字被提前折断。
    qreal naturalWidth() const;
    bool isEmpty() const;

    // origin 是内容左上角在 painter 坐标系里的位置；clip 是同一坐标系下的可见区域。
    // 传一个空的 clip 表示全画。
    void paint(QPainter* painter, const QPointF& origin, const QRectF& clip = QRectF()) const;

    // 命中测试。point 相对于**内容左上角**。没命中链接时返回空串。
    QString linkAt(const QPointF& point) const;

    // ── 选区 ────────────────────────────────────────────────
    //
    // 位置是全篇的字符偏移，坐标系就是 selectableText()。**块之间补了换行**，
    // 所以复制出来的东西保持分段，不会糊成一坨。
    int positionAt(const QPointF& point) const;
    int textLength() const;
    const QString& selectableText() const;
    // from/to 不分先后，内部会排好。from == to 表示没有选区。
    void setSelection(int from, int to);
    void clearSelection();
    QString selectedText() const;

    // 只在这个头文件里声明、在 .cpp 里定义，外面拿不到内容。
    // 放在 public 是因为 .cpp 里那个排版器要往里写东西，它不是成员。
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
