#include <QImage>
#include <QPainter>
#include <QtTest>

#include "markdown/MarkdownLayout.h"

// 排版层的测试。
//
// 这一层不是「看起来漂不漂亮」——那个只能靠眼睛。能自动测的是**结构性**的东西：
// 窄了要变高、代码块不能被挤没、链接得点得中、选区取出来的文字要对。
// 这些正是改样式时最容易悄悄弄坏的部分。
//
// 需要 QApplication：QTextLayout 要碰字体引擎。

namespace {

MarkdownLayout laidOut(const QString& markdown, qreal width) {
    MarkdownLayout layout;
    layout.layout(MarkdownDocument::parse(markdown), MarkdownTheme::standard(1.0), width);
    return layout;
}

}  // namespace

class MarkdownLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void typographyDoesNotInheritPlatformApplicationFonts();
    void emptyDocumentTakesNoSpace();
    void narrowerWidthWrapsTaller();
    void headingIsTallerThanBody();
    void codeBlockKeepsEveryLine();
    void listIndentsByDepth();
    void nestedListsUseCompactDistinctHierarchy();
    void unorderedListMarkersVaryByDepth();
    void quoteIsTallerThanItsContent();
    void linkHitTestingFindsHref();
    void selectionReadsBackTheText();
    void paintsWithoutTouchingOutsideTheClip();
    void listMarkersStayOutOfTheCopiedText();
    void deepIndentStopsGrowing();
};

void MarkdownLayoutTest::typographyDoesNotInheritPlatformApplicationFonts() {
    const QFont original = QApplication::font();
    const QString source = QStringLiteral("正文 and English **强调**\n\n`code 中文`\n\n- List");
    const auto baseline = laidOut(source, 400);
    QFont alternate(QStringLiteral("Times"));
    alternate.setWeight(QFont::Black);
    alternate.setItalic(true);
    QApplication::setFont(alternate);
    const auto changed = laidOut(source, 400);
    QApplication::setFont(original);
    QCOMPARE(changed.height(), baseline.height());
    QCOMPARE(changed.naturalWidth(), baseline.naturalWidth());
}

void MarkdownLayoutTest::emptyDocumentTakesNoSpace() {
    const MarkdownLayout layout = laidOut(QString(), 400);
    QVERIFY(layout.isEmpty());
    QCOMPARE(layout.height(), qreal(0));

    // 宽度为 0 时不能崩，也不能算出个高度来——窗口还没显示时就是这个状态。
    const MarkdownLayout zero = laidOut(QStringLiteral("有内容"), 0);
    QCOMPARE(zero.height(), qreal(0));
}

void MarkdownLayoutTest::narrowerWidthWrapsTaller() {
    // 折行是这一层最核心的行为。窄一半应该明显变高，
    // 而不是把字挤出可视区——原来用 QLabel 的 sizeHint 就栽在这儿。
    const QString text = QStringLiteral(
        "这是一段足够长的正文，长到在窄一点的容器里一定会折成好几行，"
        "用来确认排版真的按给定宽度在断行，而不是把内容溢出去。");
    const MarkdownLayout wide = laidOut(text, 600);
    const MarkdownLayout narrow = laidOut(text, 200);
    QVERIFY(wide.height() > 0);
    QVERIFY(narrow.height() > wide.height());
}

void MarkdownLayoutTest::headingIsTallerThanBody() {
    const MarkdownLayout heading = laidOut(QStringLiteral("# 标题"), 400);
    const MarkdownLayout body = laidOut(QStringLiteral("标题"), 400);
    QVERIFY(heading.height() > body.height());
}

void MarkdownLayoutTest::codeBlockKeepsEveryLine() {
    // 代码块里的空行要占位。压掉的话代码的分段就没了，读起来完全变样。
    const MarkdownLayout dense =
        laidOut(QStringLiteral("```\na\nb\n```"), 400);
    const MarkdownLayout spaced =
        laidOut(QStringLiteral("```\na\n\n\nb\n```"), 400);
    QVERIFY(spaced.height() > dense.height());

    // 复制出来的要是原文，一个字符都不改。
    QVERIFY(dense.selectableText().contains(QStringLiteral("a")));
    QVERIFY(dense.selectableText().contains(QStringLiteral("b")));
}

void MarkdownLayoutTest::listIndentsByDepth() {
    // 嵌套列表靠 depth 缩进。同样的字数，嵌套那份可用宽度更窄，所以更高。
    const QString flat = QStringLiteral("- 一段不算短的列表项文字，会折行的那种长度");
    const QString nested =
        QStringLiteral("- 外层\n  - 一段不算短的列表项文字，会折行的那种长度");
    const MarkdownLayout flatLayout = laidOut(flat, 220);
    const MarkdownLayout nestedLayout = laidOut(nested, 220);
    QVERIFY(nestedLayout.height() > flatLayout.height());
}

void MarkdownLayoutTest::nestedListsUseCompactDistinctHierarchy() {
    const QString leaf = QStringLiteral("同一段列表明细文字");
    const MarkdownLayout flat = laidOut(QStringLiteral("- ") + leaf, 600);
    const MarkdownLayout nested = laidOut(QStringLiteral("- x\n  - ") + leaf, 600);
    QVERIFY(nested.naturalWidth() - flat.naturalWidth() >= 16);

    const MarkdownLayout compact =
        laidOut(QStringLiteral("- 第一项\n- 第二项\n- 第三项\n- 第四项"), 600);
    const MarkdownTheme theme = MarkdownTheme::standard(1.0);
    const qreal fourLines = theme.bodyPixelSize * theme.lineHeightRatio * 4;
    QVERIFY(compact.height() <= fourLines + 16);
}

void MarkdownLayoutTest::unorderedListMarkersVaryByDepth() {
    const MarkdownTheme theme = MarkdownTheme::standard(1.0);
    QCOMPARE(theme.listBulletForDepth(0), QStringLiteral("•"));
    QCOMPARE(theme.listBulletForDepth(1), QStringLiteral("◦"));
    QCOMPARE(theme.listBulletForDepth(2), QStringLiteral("▪"));
    QCOMPARE(theme.listBulletForDepth(3), QStringLiteral("•"));
}

void MarkdownLayoutTest::quoteIsTallerThanItsContent() {
    // 引用要给内容留出内边距，不能贴着边。
    const MarkdownLayout quoted = laidOut(QStringLiteral("> 引用的一句话"), 400);
    const MarkdownLayout plain = laidOut(QStringLiteral("引用的一句话"), 400);
    QVERIFY(quoted.height() > plain.height());

    // 提示框还多一行标题。
    const MarkdownLayout callout =
        laidOut(QStringLiteral("> [!TIP]\n> 引用的一句话"), 400);
    QVERIFY(callout.height() > quoted.height());
}

void MarkdownLayoutTest::linkHitTestingFindsHref() {
    const MarkdownLayout layout =
        laidOut(QStringLiteral("[点我](https://example.com) 后面是普通文字"), 400);

    // 链接在第一行的最左边，取靠左的一小块一定落在链接里。
    QCOMPARE(layout.linkAt(QPointF(6, 8)), QStringLiteral("https://example.com"));
    // 行尾之外的空白不该算链接——那会让整行右边都变成可点的。
    QVERIFY(layout.linkAt(QPointF(395, 8)).isEmpty());
    // 内容下方的空白也不是链接。
    QVERIFY(layout.linkAt(QPointF(6, layout.height() + 40)).isEmpty());

    // 链接在**行尾**时最容易出事：xToCursor 会把行尾右边的空白一律映射到
    // 最后一个位置上，没有行宽判断的话整行右边都会变成可点的。
    const MarkdownLayout trailing =
        laidOut(QStringLiteral("前面是普通文字 [点我](https://example.com)"), 400);
    bool hitSomewhere = false;
    for (int x = 4; x < 400; x += 4) {
        if (!trailing.linkAt(QPointF(x, 8)).isEmpty()) hitSomewhere = true;
    }
    QVERIFY(hitSomewhere);
    QVERIFY(trailing.linkAt(QPointF(396, 8)).isEmpty());

    // 真正会出事的是**链接自己折行**：第一行是在链接中间断开的，
    // 行尾还剩一截空白，而 xToCursor 会把那截空白映射到断点上——
    // 断点在链接范围里，于是空白处也成了可点的链接。
    const MarkdownLayout wrapped = laidOut(
        QStringLiteral("[a very long link label that certainly wraps onto another line]"
                       "(https://example.com)"),
        200);
    QVERIFY(wrapped.height() > 24);  // 确实折了行
    QCOMPARE(wrapped.linkAt(QPointF(8, 8)), QStringLiteral("https://example.com"));
    QVERIFY(wrapped.linkAt(QPointF(198, 8)).isEmpty());
}

void MarkdownLayoutTest::selectionReadsBackTheText() {
    MarkdownLayout layout = laidOut(QStringLiteral("# 标题\n\n正文一段"), 400);
    const QString all = layout.selectableText();
    QVERIFY(all.contains(QStringLiteral("标题")));
    QVERIFY(all.contains(QStringLiteral("正文一段")));
    // 块之间要有换行，复制出来才是分段的。
    QVERIFY(all.contains(QStringLiteral("标题\n")));

    layout.setSelection(0, 2);
    QCOMPARE(layout.selectedText(), QStringLiteral("标题"));
    // 反着选也要能取出来——鼠标往回拖就是这种。
    layout.setSelection(2, 0);
    QCOMPARE(layout.selectedText(), QStringLiteral("标题"));
    layout.clearSelection();
    QVERIFY(layout.selectedText().isEmpty());

    // 越界的位置要被夹住，不能崩。
    layout.setSelection(-100, 100000);
    QCOMPARE(layout.selectedText(), all);
}

void MarkdownLayoutTest::paintsWithoutTouchingOutsideTheClip() {
    // 长内容滚动时靠 clip 跳过屏幕外的东西，这是不用一条消息一个部件之后
    // 性能能撑住的关键。这里验证它真的没画到可见区之外。
    const MarkdownLayout layout = laidOut(
        QStringLiteral("第一段\n\n第二段\n\n第三段\n\n第四段\n\n第五段\n\n第六段"), 400);
    QVERIFY(layout.height() > 80);

    QImage image(400, static_cast<int>(layout.height()) + 8, QImage::Format_ARGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        layout.paint(&painter, QPointF(0, 0), QRectF(0, 0, 400, 40));
    }

    bool paintedInsideClip = false;
    bool paintedOutsideClip = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) == qRgb(255, 255, 255)) continue;
            if (y < 40) {
                paintedInsideClip = true;
            } else {
                paintedOutsideClip = true;
            }
        }
    }
    QVERIFY(paintedInsideClip);
    QVERIFY(!paintedOutsideClip);

    // 可见区整个在内容外面时，一笔都不该画。
    // （跳过屏幕外内容省下的时间这里量不了，但至少能确认跳过的判断没跳错。）
    QImage below(400, 40, QImage::Format_ARGB32);
    below.fill(Qt::white);
    {
        QPainter painter(&below);
        layout.paint(&painter, QPointF(0, 0), QRectF(0, layout.height() + 100, 400, 40));
    }
    for (int y = 0; y < below.height(); ++y) {
        for (int x = 0; x < below.width(); ++x) {
            QCOMPARE(below.pixel(x, y), qRgb(255, 255, 255));
        }
    }
}

void MarkdownLayoutTest::listMarkersStayOutOfTheCopiedText() {
    // 项目符号和序号是**装饰**，不是内容。收进可选文字的话，
    // 复制一段列表会变成每个符号单占一行——有序列表原来就是这样的。
    const MarkdownLayout layout =
        laidOut(QStringLiteral("- 第一项\n- 第二项\n\n1. 甲\n2. 乙"), 400);
    const QString text = layout.selectableText();

    QVERIFY(text.contains(QStringLiteral("第一项")));
    QVERIFY(text.contains(QStringLiteral("乙")));
    QVERIFY(!text.contains(QStringLiteral("•")));
    QVERIFY(!text.contains(QStringLiteral("1.")));
    QVERIFY(!text.contains(QStringLiteral("2.")));

    // 标记列要按整张列表里最宽的那个标记撑开：16px 放不下「10.」，
    // 定宽的话它会被折成两行，而且同一张列表各项的正文左边对不齐。
    //
    // 量的是**正文那一列往右挪了多少**（naturalWidth 里含每段的起点 x）。
    // 不用高度：高度得靠折行数变化才看得出来，而那取决于字号和宽度凑得准不准，
    // 字号一调断言就恒真了——正文从 15 改到 14 的时候就是这么失效的。
    // 也不能直接比「10.」有没有折行：标记的高度根本不参与行高计算。
    const MarkdownLayout narrowMarker = laidOut(QStringLiteral("1. 甲"), 400);
    const MarkdownLayout wideMarker = laidOut(QStringLiteral("10. 甲"), 400);
    QVERIFY(wideMarker.naturalWidth() > narrowMarker.naturalWidth());

    // 提示框的中文标题是生成出来的，同样不该混进复制的内容里。
    const MarkdownLayout callout =
        laidOut(QStringLiteral("> [!TIP]\n> 记得备份"), 400);
    QVERIFY(callout.selectableText().contains(QStringLiteral("记得备份")));
}

void MarkdownLayoutTest::deepIndentStopsGrowing() {
    // 缩进要封顶。不封的话十层嵌套会把正文推到列外面，每行只剩两三个字。
    //
    // 只断言「封顶以上高度相同」是不够的——叶子要是在两种宽度下都折成一样多行，
    // 那条断言恒真。所以**成对**断言：封顶以下要变，封顶以上不变。

    // 量某个深度下叶子那一项占了多高：整篇减去同样结构但叶子只有一个字的那篇。
    const auto leafHeight = [](int depth, const QString& leaf) {
        QString source;
        for (int level = 0; level < depth; ++level) {
            source += QString(level * 2, QLatin1Char(' ')) + QStringLiteral("- a%1").arg(level) +
                      QLatin1Char('\n');
        }
        source += QString(depth * 2, QLatin1Char(' ')) + QStringLiteral("- ") + leaf;
        // 宽度取窄：列越窄，每层缩进少掉的那 12px 占比越大，
        // 行数差得出来。用 400 宽的话多缩进三层也还是折一样多行，断言就恒真了。
        return laidOut(source, 180).height();
    };

    const QString leaf = QStringLiteral("一段够长的叶子节点文字用来看还剩多少宽度可用啊");
    const QString tiny = QStringLiteral("x");

    const qreal shallow = leafHeight(1, leaf) - leafHeight(1, tiny);
    const qreal atCap = leafHeight(4, leaf) - leafHeight(4, tiny);
    const qreal beyondCap = leafHeight(8, leaf) - leafHeight(8, tiny);

    QVERIFY(shallow > 0);
    // 封顶以下：越深越窄，叶子越高。这条保证上面那条不是恒真的。
    QVERIFY(atCap > shallow);
    // 封顶以上：不再变窄。
    QCOMPARE(beyondCap, atCap);
}

QTEST_MAIN(MarkdownLayoutTest)
#include "MarkdownLayoutTest.moc"
