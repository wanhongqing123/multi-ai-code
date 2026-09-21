#pragma once

#include <QString>
#include <QWidget>
#include <memory>

#include "markdown/MarkdownTheme.h"

// 一段 Markdown，**不滚动，高度跟着内容走**。
//
// 和 MarkdownView 的分工：
//
//   MarkdownView    整个展示区是一个部件，自己管滚动，一条消息一个条目。
//                   AI 助手页那种「一列内容从上到下」的场景。
//   MarkdownLabel   就一段内容，嵌在别人的布局里，高度由 heightForWidth 给。
//                   IM 的消息气泡——气泡本身是行布局里的部件，滚动是外面
//                   那个 QScrollArea 在管，这里再套一层滚动区是错的。
//
// 两者共用 MarkdownDocument / MarkdownLayout / MarkdownTheme，所以
// **IM 和 AI 助手的 markdown 是同一套渲染**：调一处，两边一起变。
//
// 背景是透明的：气泡的底色、圆角、外边距都归外面那层管，这里只画字。
//
// 线程：只在界面线程用。
class MarkdownLabel : public QWidget {
    Q_OBJECT

public:
    explicit MarkdownLabel(QWidget* parent = nullptr);
    ~MarkdownLabel() override;

    void setTheme(const MarkdownTheme& theme);
    const MarkdownTheme& theme() const;

    // 原文。渲染是有损的（标题、代码围栏、链接目标都还原不回来），
    // 所以原文单独留着，「复制原始数据」用的是它。
    void setMarkdown(const QString& markdown);
    const QString& markdown() const;

    // 末尾那一小段附注：IM 的消息时间戳。接在正文最后一行的末尾，不另起一行。
    // 要在 setMarkdown 之前或之后设都行，两个都会触发重排。
    void setTrailingNote(const QString& text, const QColor& color, int pixelSize);

    // 画出来的可见文字，含末尾附注。相当于原来 QTextBrowser 的 toPlainText()。
    QString plainText() const;

    // 当前宽度下的内容高度。宿主拿它去 setFixedHeight。
    int contentHeight() const;

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // 命中测试，部件坐标。没命中返回空串。
    QString linkAt(const QPoint& point) const;

    QString selectedText() const;
    bool hasSelection() const;
    void selectAll();
    void clearSelection();

public slots:
    void copySelection();

signals:
    void linkActivated(const QString& href);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // 内容或皮肤变了：只作废，不排版。真要用的时候再排。
    void invalidate();
    // 把排好的版交还给缓存，下次还能用上。
    void returnLayoutToCache();
    void returnProbeToCache();
    // 按 width 排版；宽度没变就是空操作。
    void ensureLayout(qreal width);
    // 按**当前部件宽度**排版。量高度用的是另一份版，不会影响这份。
    void ensureCurrentLayout() const;

    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
