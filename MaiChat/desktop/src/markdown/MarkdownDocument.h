#pragma once

#include <QFlags>
#include <QString>
#include <QVector>

// Markdown 解析出来的**块树**。纯数据，不碰任何界面类型。
//
// ── 为什么要有它 ────────────────────────────────────────────────
//
// 原来的路线是 md4c → HTML → QTextDocument，绘制全交给 Qt。省事，但受 QTextDocument
// 的 CSS 子集限制：**不支持行内 padding、块级圆角、border-left、复选框**。
// MarkdownRenderer.cpp 里那些「用 table 模拟引用块」「用 bgcolor 表格模拟代码块」
// 的后处理就是被这个逼出来的，而且再往上走（代码块圆角、语法高亮、可折叠的长输出）
// 全都做不到。
//
// 所以改成自己画。这个文件是那条路的第一步：把 Markdown 变成一棵能直接用来排版的树。
//
// ── 为什么块树和绘制要分开 ──────────────────────────────────────
//
// 分开之后这一层是**纯函数**：一段文本进去，一棵树出来，没有 QWidget、没有 QPainter、
// 不需要跑起界面就能测。Markdown 的坑几乎全在解析和结构上（嵌套列表、
// 代码块里的反引号、表格对齐、软换行），那些用例在这一层写，跑得又快又稳。
//
// 绘制那一层（MarkdownView）只负责把树摆到像素上，它的 bug 是"看起来不对"，
// 得靠眼睛；两种 bug 混在一个文件里会互相掩盖。

// 行内样式。用位标志而不是枚举：**样式是会叠加的**，
// 「加粗的链接」「链接里的行内代码」都很常见，一个片段可能同时带好几种。
enum class MarkdownStyle {
    Normal = 0x0,
    Bold = 0x1,
    Italic = 0x2,
    Code = 0x4,
    Strike = 0x8,
    Link = 0x10,
};
Q_DECLARE_FLAGS(MarkdownStyles, MarkdownStyle)
Q_DECLARE_OPERATORS_FOR_FLAGS(MarkdownStyles)

// 一段样式一致的文本。一个段落由若干个这种片段拼成。
struct MarkdownSpan {
    QString text;
    MarkdownStyles styles = MarkdownStyle::Normal;
    // 只在带 Link 时有效。**解析阶段就把不安全的协议摘掉**（只留 http/https/mailto/锚点），
    // 别指望绘制层去防——那一层只管画，不该承担安全判断。
    QString href;
};

// 列表里的一项。
struct MarkdownListItem {
    QVector<MarkdownSpan> spans;
    // 嵌套深度，0 是最外层。**扁平存放而不是递归**：绘制时只需要知道缩进多少，
    // 递归结构反而要在画的时候再展平一次。
    int depth = 0;
    // 任务列表（- [x] / - [ ]）。hasCheckbox 为 false 时 checked 无意义。
    bool hasCheckbox = false;
    bool checked = false;
    // 有序列表里显示的序号。无序列表恒为 0。
    int number = 0;
};

enum class MarkdownBlockKind {
    Paragraph,
    Heading,
    List,      // 有序无序都是它，看 MarkdownListItem::number
    Code,
    Divider,
    Table,
};

// GFM 的提示框：`> [!NOTE]` 开头的引用。老的 HTML 渲染器认这个并且给了中文标题，
// 换成自绘后不认就是退化，所以在**解析阶段**就识别出来。
enum class MarkdownCallout {
    None,
    Note,
    Tip,
    Important,
    Warning,
    Caution,
};

struct MarkdownBlock {
    MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;

    // Paragraph / Heading / Quote 用。
    QVector<MarkdownSpan> spans;
    // Heading 用，1..6。
    int headingLevel = 0;

    // Code 用。code 是**原文**，不做任何转义——绘制层要按等宽字体逐行画，
    // 而且「复制」按钮复制的就是它。
    QString code;
    QString language;

    // List 用。
    QVector<MarkdownListItem> items;

    // Table 用：rows[0] 是表头。每个单元格是一串片段（单元格里可以有粗体和行内代码）。
    QVector<QVector<QVector<MarkdownSpan>>> rows;

    // 这个块外面套着哪几层引用，从最外层排到最里层，每层一个编号。
    //
    // **引用不是一种块，是一个标记。** 它里面能装段落、列表、代码甚至再套一层引用，
    // 做成「带竖条的一段文字」就把这些全压平了。这里和列表用同一个办法：扁平存，
    // 标记写在每个块上，绘制时把**同属一个引用**的块归成一组画背景和竖条。
    //
    // 存的是编号而不只是深度：相邻的两个 `>` 是**两个**框，深度却一样。
    // 只看深度的话它们会被合成一个，各自的提示框类型也只剩第一个那份。
    QVector<int> quoteIds;
    // 提示框类型。挂在这个引用里的每个块上，绘制时取组里第一个。
    MarkdownCallout callout = MarkdownCallout::None;

    int quoteDepth() const {
        return quoteIds.size();
    }
};

// 提示框的中文标题：「建议」「注意」…… 绘制时画在框头上，
// 会话列表的预览行也拿它当前缀。**只有这一份**，主题那边取的就是它。
QString markdownCalloutTitle(MarkdownCallout callout);

// 一整篇。
class MarkdownDocument {
public:
    // 解析。方言和原来那条路保持一致（GFM：表格、删除线、任务列表、自动链接），
    // 否则同一段文字在 agent 和 IM 里会渲染成两样。
    static MarkdownDocument parse(const QString& markdown);

    const QVector<MarkdownBlock>& blocks() const {
        return mBlocks;
    }

    // **原文要留着。** IM 的右键菜单有一条「复制原始数据」，复制的是 Markdown 原文
    // 而不是渲染结果——渲染是有损的，标题、代码围栏、链接目标都还原不回来。
    const QString& source() const {
        return mSource;
    }

    // 摊平成纯文本。会话列表的消息预览要它：那一行只有几十个字符，
    // 不该带任何样式，也不该把「**粗体**」的星号显示出来。
    QString plainText() const;

    bool isEmpty() const {
        return mBlocks.isEmpty();
    }

private:
    QVector<MarkdownBlock> mBlocks;
    QString mSource;
};
