#include "markdown/MarkdownTheme.h"

namespace {

// 和 UiZoom::s 一样的取整规则：0 还是 0，非零最小 1px，不被缩没。
int scale(int px, qreal zoom) {
    if (px == 0) return 0;
    return qMax(1, qRound(px * zoom));
}

QColor hex(const char* value) {
    return QColor(QString::fromLatin1(value));
}

// 等宽字体按平台给一个确定的名字。写成浏览器式的回退列表没用——
// QFont 不解析那种写法，Qt 只认单个族名。
QString monospaceFamily() {
#ifdef Q_OS_MAC
    return QStringLiteral("Menlo");
#elif defined(Q_OS_WIN)
    return QStringLiteral("Consolas");
#else
    return QStringLiteral("monospace");
#endif
}

}  // namespace

MarkdownTheme MarkdownTheme::standard(qreal zoom) {
    MarkdownTheme theme;
    // 记下倍率：它同时是这份皮肤的指纹，排版缓存靠它分辨两份皮肤。
    theme.zoom = zoom;
    theme.codeFamily = monospaceFamily();
    // 标题字号按 20/17/15/14/14/14 收紧。老的 IM 值是 22/18/16，那是给
    // 「一两句话里偶尔一个标题」用的；长回答里 22px 的 h1 太抢眼。


    theme.text = hex("#0e1525");
    // **粗体、斜体、删除线都不改颜色。**
    //
    // iOS 那边只给行内代码换色，其余全靠字形本身的差别（粗、斜、删除线）。
    // 颜色和字形双重编码没必要，而且一段话里深深浅浅好几种灰反而更花。
    theme.strong = hex("#0e1525");
    theme.emphasis = hex("#0e1525");
    theme.strike = hex("#0e1525");
    theme.link = hex("#0b67b7");
    // 标题只有 h1/h2 上色，用一个沉的藏青；h3 往下就是正文墨色。
    //
    // 老的 IM 样式是 h2 亮蓝 #1769be、h3 青 #176e83，一屏五个标题就成了圣诞树。
    // iOS 的做法是中间路线：只把最高的两级挑出来，而且颜色压得很沉。
    theme.headingColor[0] = hex("#1c4f8a");
    theme.headingColor[1] = hex("#1c4f8a");
    theme.headingColor[2] = hex("#0e1525");
    theme.headingColor[3] = hex("#0e1525");
    theme.headingColor[4] = hex("#0e1525");
    theme.headingColor[5] = hex("#0e1525");
    // 行内代码是**紫**的，照 iOS。蓝色会和链接撞，中性灰又太弱看不出这是代码。
    theme.inlineCodeText = hex("#6b3b97");
    theme.inlineCodeBackground = hex("#f2edfa");
    theme.codeText = hex("#0e1525");
    theme.codeBackground = hex("#f4f6f9");
    theme.codeBorder = hex("#e5ecf5");
    theme.quoteAccent = hex("#2873c7");
    theme.quoteBackground = hex("#eaf3fd");
    theme.quoteText = hex("#475569");
    // 表格对齐 iOS：**浅蓝灰表头配深色文字**，不是深蓝配白字。
    // 深色表头在一屏里比正文还抢眼，而表格通常只是个附注。
    theme.tableHeaderBackground = hex("#e8f0f7");
    theme.tableHeaderText = hex("#0e1525");
    theme.tableRow = hex("#ffffff");
    theme.tableRowAlternate = hex("#f5f7fa");
    theme.tableLine = hex("#dae4f0");
    // 行间那条线比外框还淡（iOS 是 border 的 60% 透明度压在白底上）。
    theme.tableSeparator = hex("#e9eff6");
    theme.divider = hex("#e2e8f0");
    // 项目符号和序号都用主蓝，和 iOS 一致。原来是灰点，在一堆黑字里看不见。
    theme.listMarker = hex("#0f8ddd");
    theme.checkboxOn = hex("#17826b");
    theme.checkboxOff = hex("#64758f");
    theme.selection = hex("#b4d5fe");
    theme.selectionText = hex("#102a43");
    theme.bubbleBackground = hex("#e8f2ff");
    theme.bubbleText = hex("#16314f");
    theme.noticeText = hex("#8b96a5");
    theme.errorText = hex("#c0392b");
    theme.viewBackground = hex("#ffffff");
    // 和 MaiChat 会话列表那两条细滚动条同色，不另起一套。
    theme.scrollBarHandle = hex("#d3dae4");
    theme.scrollBarHandleHover = hex("#b9c3d1");

    // 缩放放在最后统一做，上面那些值就能和 CSS 逐条对着看。
    theme.bodyPixelSize = scale(theme.bodyPixelSize, zoom);
    theme.codePixelSize = scale(theme.codePixelSize, zoom);
    theme.codeBlockPixelSize = scale(theme.codeBlockPixelSize, zoom);
    theme.tablePixelSize = scale(theme.tablePixelSize, zoom);
    theme.listMarkerPixelSize = scale(theme.listMarkerPixelSize, zoom);
    theme.listBulletPixelSize = scale(theme.listBulletPixelSize, zoom);
    theme.calloutTitlePixelSize = scale(theme.calloutTitlePixelSize, zoom);
    for (int level = 0; level < 6; ++level) {
        theme.headingPixelSize[level] = scale(theme.headingPixelSize[level], zoom);
        theme.headingSpacingAbove[level] = scale(theme.headingSpacingAbove[level], zoom);
        theme.headingSpacingBelow[level] = scale(theme.headingSpacingBelow[level], zoom);
    }
    theme.blockSpacing = scale(theme.blockSpacing, zoom);
    theme.listIndent = scale(theme.listIndent, zoom);
    theme.listMarkerWidth = scale(theme.listMarkerWidth, zoom);
    theme.listMarkerGap = scale(theme.listMarkerGap, zoom);
    theme.listItemSpacing = scale(theme.listItemSpacing, zoom);
    theme.codePadding = scale(theme.codePadding, zoom);
    theme.codeRadius = scale(theme.codeRadius, zoom);
    theme.quoteBarWidth = scale(theme.quoteBarWidth, zoom);
    theme.quotePadding = scale(theme.quotePadding, zoom);
    theme.quoteRadius = scale(theme.quoteRadius, zoom);
    theme.dividerSpacing = scale(theme.dividerSpacing, zoom);
    theme.tableCellPaddingH = scale(theme.tableCellPaddingH, zoom);
    theme.tableCellPaddingV = scale(theme.tableCellPaddingV, zoom);
    theme.tableRadius = scale(theme.tableRadius, zoom);
    theme.checkboxSize = scale(theme.checkboxSize, zoom);
    theme.viewMargin = scale(theme.viewMargin, zoom);
    theme.scrollBarWidth = scale(theme.scrollBarWidth, zoom);
    theme.itemSpacing = scale(theme.itemSpacing, zoom);
    theme.bubblePadding = scale(theme.bubblePadding, zoom);
    theme.bubbleRadius = scale(theme.bubbleRadius, zoom);
    return theme;
}

MarkdownCalloutStyle MarkdownTheme::calloutStyle(MarkdownCallout callout) const {
    switch (callout) {
        // 标题不在这儿写死：会话列表的预览行也要同一串字，
        // 两处各写一份迟早会对不上。
        case MarkdownCallout::Note:
            return {hex("#1f64b0"), hex("#f0f5fb"), markdownCalloutTitle(callout)};
        case MarkdownCallout::Tip:
            return {hex("#16836b"), hex("#eff7f4"), markdownCalloutTitle(callout)};
        case MarkdownCallout::Important:
            return {hex("#7547a8"), hex("#f5f1fa"), markdownCalloutTitle(callout)};
        case MarkdownCallout::Warning:
            return {hex("#9c640f"), hex("#fbf6ec"), markdownCalloutTitle(callout)};
        case MarkdownCallout::Caution:
            return {hex("#ba3d40"), hex("#fbf1f2"), markdownCalloutTitle(callout)};
        case MarkdownCallout::None:
            break;
    }
    return {quoteAccent, quoteBackground, QString()};
}
