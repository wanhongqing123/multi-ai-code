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
    // ── 字体 ────────────────────────────────────────────────
    // 空表示跟随应用默认字体。等宽字体按平台给，不写浏览器式的回退列表。
    QString bodyFamily;
    QString codeFamily;
    int bodyPixelSize = 15;
    int codePixelSize = 13;
    int tablePixelSize = 12;
    // 列表序号单独一个字号，比正文小一号并加粗——照 iOS 的 13 semibold。
    int listMarkerPixelSize = 13;
    // 项目符号要比序号大一点。iOS 那边符号和序号同是 13，但 SF Pro 的 U+2022
    // 比 Segoe UI 的粗不少，照搬字号出来的点发虚、在一堆黑字里看不见。
    // 字号是为了配那个字体定的，要对齐的是**看起来一样重**，不是数字一样。
    int listBulletPixelSize = 16;
    int calloutTitlePixelSize = 13;
    int headingPixelSize[6] = {22, 18, 16, 14, 14, 14};
    // 行高倍率。原来交给 QTextDocument 默认值，偏挤；聊天里长段落多，放宽一点。
    qreal lineHeightRatio = 1.45;

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
    int blockSpacing = 10;
    // 标题上下的留白。和 CSS 的 margin 一样会**合并**：相邻两块之间取两者的大值，
    // 不是相加——不然标题跟在段落后面时会空出一大块。
    int headingSpacingAbove[6] = {0, 16, 12, 10, 10, 10};
    int headingSpacingBelow[6] = {12, 10, 8, 8, 8, 6};
    // 列表的三个横向量是分开的，别拿一个数兼着用：
    //   listIndent       每嵌套一层往右挪多少
    //   listMarkerWidth  标记列的宽度，标记在里面**右对齐**
    //   listMarkerGap    标记列到正文的间隙
    // 合成一个数的话，缩进一深正文就被推得老远——iOS 是 12 / 16 / 8。
    int listIndent = 12;
    int listMarkerWidth = 16;
    int listMarkerGap = 8;
    // 缩进封顶。再深就不往右挪了，否则窄一点的列宽下正文没地方站。
    int listMaxDepth = 4;
    int listItemSpacing = 8;
    int codePadding = 10;
    int codeRadius = 8;
    int quoteBarWidth = 3;
    int quotePadding = 10;
    int quoteRadius = 8;
    int dividerSpacing = 8;
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

    // 标准主题。zoom 传 UiZoom::factor()；非界面场景（比如测量）传 1.0。
    static MarkdownTheme standard(qreal zoom);

    // 提示框（> [!NOTE] 之类）的配色和中文标题。None 返回普通引用的样式。
    MarkdownCalloutStyle calloutStyle(MarkdownCallout callout) const;
};
