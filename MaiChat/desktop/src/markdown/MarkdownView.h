#pragma once

#include <QAbstractScrollArea>
#include <QRectF>
#include <QString>
#include <memory>

#include "markdown/MarkdownTheme.h"

// 一整个消息展示区，**就这一个部件**。
//
// ── 为什么不是一条消息一个部件 ──────────────────────────────────
//
// 原来每条消息一个 QLabel / QTextBrowser。部件是重的：每个都要参与布局、
// 样式表匹配、事件分发，几百条之后滚动会明显卡，而且窗口一变宽就整列重算。
// 这里一条消息只是一棵块树加一份排好的版，绘制时按可见区裁剪——
// **屏幕外的消息一笔都不画**。
//
// 附带解决的两件事：
//
//   跨消息选区   原来每个 QTextBrowser 各管各的，选不过界。现在整片是一个
//                坐标系，从第一条拖到第十条是自然的。
//   一致的外观   agent 和 IM 用同一个 MarkdownView 和同一份 MarkdownTheme，
//                不会一边改了另一边忘了。
//
// ── 嵌部件 ──────────────────────────────────────────────────────
//
// 工具卡那种带按钮的东西画不出来（要能点），所以留了 addWidget：视图负责
// 摆位置、跟着滚、滚出去就隐藏，部件自己管交互。iOS 那边 UITableView 里
// 混自定义 cell 是一样的做法。
//
// 线程：只在界面线程用。

// 视图里的一条内容。定义在 .cpp 里，这儿只要个名字给私有成员用。
struct MarkdownViewItem;

class MarkdownView final : public QAbstractScrollArea {
    Q_OBJECT

public:
    // 一条内容长什么样。**不是内容类型，是表现形态**——
    // 同一段 Markdown 放进气泡和铺满整列是两种读法。
    enum class Style {
        Document,  // 整列宽的正文。模型的回答、IM 里收到的长消息
        Bubble,    // 右侧气泡。人发出去的短消息
        Notice,    // 居中的淡色小字。「已清空」之类
        Error,     // 居中的红字
    };

    explicit MarkdownView(QWidget* parent = nullptr);
    ~MarkdownView() override;

    // 换皮肤。会整片重排。
    void setTheme(const MarkdownTheme& theme);
    const MarkdownTheme& theme() const;

    // 阅读列的最大宽度。窗口拉到 2000px 宽时正文不该跟着拉那么宽——
    // 一行太长，眼睛回扫的时候会丢行。超出的部分留白，内容**居中**。
    // 0 表示不限制（IM 的气泡列表就不需要限）。
    void setMaxContentWidth(int width);

    // id 由调用方给，流式更新和删除都按它找。重复的 id 会被当成更新。
    void addItem(const QString& id, Style style, const QString& markdown);
    // 流式期间只改这一条，其余的版不动。
    void updateItem(const QString& id, const QString& markdown);
    bool contains(const QString& id) const;
    void removeItem(const QString& id);

    // 嵌一个部件。所有权转给视图。
    void addWidget(const QString& id, QWidget* widget);

    void clear();
    bool isEmpty() const;
    int itemCount() const;

    // 某条内容的外框，**内容坐标系**（y 从整片顶上算，和滚动位置无关）。
    // 找不到时返回空矩形。用来定位到某条消息，也是测试量几何的入口。
    QRectF itemRect(const QString& id) const;
    qreal contentHeight() const;

    // 贴底。内容在长的时候会自动保持贴底，用户手动往上滚之后就不再自动跟了——
    // 正在回看历史时被拽回底部是很烦的。
    void scrollToBottom();
    bool isAtBottom() const;

    // 选中的文字。跨消息，按显示顺序拼。
    QString selectedText() const;
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
    void contextMenuEvent(QContextMenuEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    // 嵌进来的部件自己变高时（工具卡展开、思考条收起）要跟着重排。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 整片重排。**每次都整片**：条目之间的 y 是连着的，改一条后面全要挪。
    // 流式输出时靠调用方节流；真成瓶颈再改成只重排变了的那条加后面的偏移。
    void relayoutAll();
    void updateScrollRange();
    void positionWidgets();
    void applySelectionToItems();
    void applyScrollBarStyle();

    // viewportPoint 是视口坐标。local 回填成相对那条内容左上角的坐标。
    const MarkdownViewItem* itemAt(const QPoint& viewportPoint, QPointF* local = nullptr) const;
    int positionAt(const QPoint& viewportPoint) const;

    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
