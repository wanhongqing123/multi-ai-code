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
    int tablePixelSize = 13;
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
    QColor bullet;
    QColor checkboxOn;
    QColor checkboxOff;
    QColor selection;
    QColor selectionText;

    // ── 间距 ────────────────────────────────────────────────
    int blockSpacing = 10;
    // 标题上下的留白。和 CSS 的 margin 一样会**合并**：相邻两块之间取两者的大值，
    // 不是相加——不然标题跟在段落后面时会空出一大块。
    int headingSpacingAbove[6] = {0, 16, 12, 10, 10, 10};
    int headingSpacingBelow[6] = {12, 10, 8, 8, 8, 6};
    int listIndent = 22;
    int listItemSpacing = 4;
    int codePadding = 10;
    int codeRadius = 8;
    int quoteBarWidth = 3;
    int quotePadding = 10;
    int quoteRadius = 8;
    int dividerSpacing = 8;
    int tableCellPadding = 8;
    int checkboxSize = 13;
    int bulletRadius = 3;

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
