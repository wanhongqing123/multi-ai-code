#pragma once

#include <QColor>
#include <QString>

#include "markdown/MarkdownDocument.h"

// Markdown 的配色和尺寸，**两端共用一套**。
//
// 值是从原来 MarkdownRenderer.cpp 里那段 CSS 一条条搬过来的，不是我另编的：
// 换成自绘是为了突破 QTextDocument 的 CSS 子集，不是为了顺手改一遍外观。
// 外观要调的话在这里调一次，agent 和 IM 一起变——这正是用户要的
// 「肯定得全都公用一套」。
//
// 尺寸都是**逻辑像素**，standard(zoom) 会按倍率缩好，调用方直接用。
//
// 倍率是**必填**而不是默认 1.0：这一层刻意不认识 UiZoom——认了它就得依赖
// remote_im_desktop_ui，而那一层反过来要用这里的绘制组件，成环。参数不给默认值，
// 调用方就没法忘。界面里一律传 UiZoom::factor()。

struct MarkdownCalloutStyle {
    QColor accent;
    QColor background;
    QString title;
};

struct MarkdownTheme {
    // 无序列表按层级循环使用实心圆、空心圆和方块，与浏览器默认的
    // disc / circle / square 层级一致。绘制层不要把所有深度重新压成一种标记。
    QString listBulletForDepth(int depth) const;

    // ── 字体 ────────────────────────────────────────────────
    // 空表示跟随应用默认字体。等宽字体按平台给，不写浏览器式的回退列表。
    QString bodyFamily;
    QString codeFamily;
    int bodyPixelSize = 14;
    // 行内代码和代码块**不是一个字号**：iOS 行内 13、块内 12。
    // 行内代码嵌在正文里，小太多会显得塌下去；代码块整块都是等宽字，小一点更紧凑。
    int codePixelSize = 13;
    int codeBlockPixelSize = 12;
    int tablePixelSize = 12;
    // 列表序号单独一个字号，比正文小一号并加粗——照 iOS 的 13 semibold。
    int listMarkerPixelSize = 13;
    // 项目符号保持小而实，避免大圆点比正文更抢眼。
    int listBulletPixelSize = 13;
    int calloutTitlePixelSize = 13;
    int headingPixelSize[6] = {22, 18, 16, 14, 14, 14};
    // 行高倍率。
    //
    // iOS 是 14pt 字加 4pt 行距，算下来约 1.48。桌面的阅读列有 760px，
    // 比手机宽一倍多，理论上该更松一点；但两端「公用一套」的前提下先对齐 iOS，
    // 真读着累再单独给桌面加个系数。
    qreal lineHeightRatio = 1.48;

    // ── 颜色 ────────────────────────────────────────────────
    QColor text;
    QColor strong;
    QColor emphasis;
    QColor strike;
    QColor link;
    QColor headingColor[6];
    QColor inlineCodeText;
    QColor inlineCodeBackground;
    QColor codeText;
    QColor codeBackground;
    QColor codeBorder;
    QColor quoteAccent;
    QColor quoteBackground;
    QColor quoteText;
    QColor tableHeaderBackground;
    QColor tableHeaderText;
    QColor tableRow;
    QColor tableRowAlternate;
    QColor tableLine;
    QColor divider;
    // 项目符号和序号是**同一个颜色**（iOS 两者都用主蓝），不再分开。
    QColor listMarker;
    QColor checkboxOn;
    QColor checkboxOff;
    QColor tableSeparator;
    QColor selection;
    QColor selectionText;

    // ── 间距 ────────────────────────────────────────────────
    // 块与块之间。iOS 的 VStack 是 12。
    int blockSpacing = 12;
    // 标题上下的留白。和 CSS 的 margin 一样会**合并**：相邻两块之间取两者的大值，
    // 不是相加——不然标题跟在段落后面时会空出一大块。
    // iOS 是在 12 的块间距之上，给 h1/h2 再加 4、h3 往下再加 2。
    int headingSpacingAbove[6] = {16, 16, 14, 14, 14, 14};
    int headingSpacingBelow[6] = {12, 12, 12, 12, 12, 12};
    // 列表的三个横向量是分开的，别拿一个数兼着用：
    //   listIndent       每嵌套一层往右挪多少
    //   listMarkerWidth  标记列的宽度，标记在里面**右对齐**
    //   listMarkerGap    标记列到正文的间隙
    // 12px 在桌面宽屏上几乎看不出层级；提高层级步进，标记列和正文间距不变。
    int listIndent = 18;
    int listMarkerWidth = 16;
    int listMarkerGap = 8;
    // 缩进封顶。再深就不往右挪了，否则窄一点的列宽下正文没地方站。
    int listMaxDepth = 4;
    int listItemSpacing = 4;
    int codePadding = 12;
    int codeRadius = 10;
    int quoteBarWidth = 3;
    int quotePadding = 12;
    int quoteRadius = 8;
    // 分割线自己只要一点点留白，块间距已经给了 12。
    int dividerSpacing = 3;
    // 单元格的横竖内边距不一样：横向要宽一点，列之间才分得开。
    int tableCellPaddingH = 12;
    int tableCellPaddingV = 10;
    // 表格整体圆角。表头和末行要**跟着裁**，不然方角会戳出圆角外面。
    int tableRadius = 10;
    int checkboxSize = 13;

    // ── 消息列表（MarkdownView 用）────────────────────────
    //
    // 气泡、提示行这些不属于 Markdown 本身，但**属于同一套外观**：
    // 放在这里，agent 和 IM 换皮肤时只改一个地方。
    QColor bubbleBackground;
    QColor bubbleText;
    QColor noticeText;
    QColor errorText;
    QColor viewBackground;
    int viewMargin = 16;
    int scrollBarWidth = 8;
    QColor scrollBarHandle;
    QColor scrollBarHandleHover;
    int itemSpacing = 14;
    int bubblePadding = 10;
    int bubbleRadius = 10;
    // 气泡最宽占可用宽度的多少。人发的消息短，拉满一行反而难读。
    qreal bubbleMaxWidthRatio = 0.72;

    // 造这份皮肤用的缩放倍率。**同时当这份皮肤的指纹**：排好的版按它缓存
    // （见 MarkdownLayoutCache），两份皮肤只要这个数一样就会被当成同一份。
    //
    // 产品里所有主题都出自 standard(zoom)，所以这个数就够用了。要是哪天有人
    // 手改了别的字段又不动这里，缓存会命中旧的那份——那就把这里也改掉。
    qreal zoom = 1.0;

    // 标准主题。zoom 传 UiZoom::factor()；非界面场景（比如测量）传 1.0。
    static MarkdownTheme standard(qreal zoom);

    // 提示框（> [!NOTE] 之类）的配色和中文标题。None 返回普通引用的样式。
    MarkdownCalloutStyle calloutStyle(MarkdownCallout callout) const;
};
