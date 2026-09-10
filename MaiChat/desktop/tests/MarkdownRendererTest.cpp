#include <QTest>

#include "markdown/MarkdownRenderer.h"
#include "markdown/PreviewText.h"

class MarkdownRendererTest : public QObject {
    Q_OBJECT

private slots:
    void rendersCommonAiCliMarkdown();
    void collapsesSoftLineBreaksLikeElectron();
    void escapesRawHtmlAndUnsafeLinks();
    void rendersFencedCodeWithoutInlineFormatting();
    void rendersGfmTableStrikethroughTaskList();
    void rendersQuoteAccentWithoutDroppingText();
    void rendersSemanticCallouts_data();
    void rendersSemanticCallouts();
    void preservesLiteralCalloutMarkers();
    void stylesTaskMarkersAndPreservesCodeLines();
    void rendersCalloutsWithMixedLineEndingsAndNestedQuotes();
    void toleratesCalloutMarkerCaseAndTrailingSpaces();
    void previewHtmlPreservesCodeSemanticsWithoutFontHeuristics();
    void previewRejectsOldQtBoundariesInsideEmoji();
};

void MarkdownRendererTest::rendersCommonAiCliMarkdown() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("## 标题\n\n**重点** 和 `code`\n\n- 第一条\n- [链接](https://example.com)"));

    QVERIFY(html.contains(QStringLiteral("<h2>标题</h2>")));
    QVERIFY(html.contains(QStringLiteral("<strong>重点</strong>")));
    // 行内代码带 thin-space(&#8201;) 模拟 chip 内边距（Qt 不支持行内 padding）。
    QVERIFY(html.contains(QStringLiteral("<code>&#8201;code&#8201;</code>")));
    QVERIFY(html.contains(QStringLiteral("<ul>")));
    QVERIFY(html.contains(QStringLiteral("<li>第一条</li>")));
    QVERIFY(html.contains(QStringLiteral("href=\"https://example.com\"")));
}

void MarkdownRendererTest::collapsesSoftLineBreaksLikeElectron() {
    // CommonMark 软换行折叠为空格——与 Electron 端 react-markdown 行为一致；
    // 硬换行（行尾两个空格）仍然是 <br />。
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("line 1\nline 2"));
    QVERIFY(html.contains(QStringLiteral("line 1\nline 2")));
    QVERIFY(!html.contains(QStringLiteral("<br")));

    const QString hardBreak = MarkdownRenderer::renderToHtml(QStringLiteral("line 1  \nline 2"));
    QVERIFY(hardBreak.contains(QStringLiteral("<br />")));
}

void MarkdownRendererTest::escapesRawHtmlAndUnsafeLinks() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("<script>alert(1)</script>\n[x](javascript:alert(1))"));

    QVERIFY(html.contains(QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt;")));
    QVERIFY(!html.contains(QStringLiteral("<script>")));
    QVERIFY(!html.contains(QStringLiteral("javascript:")));
    QVERIFY(!html.contains(QStringLiteral("href=\"javascript:")));
}

void MarkdownRendererTest::rendersFencedCodeWithoutInlineFormatting() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("```cpp\n**not bold** <tag>\n```"));

    QVERIFY(html.contains(QStringLiteral("<pre><code class=\"language-cpp\">**not bold** &lt;tag&gt;")));
    QVERIFY(!html.contains(QStringLiteral("<strong>not bold</strong>")));
}

void MarkdownRendererTest::rendersGfmTableStrikethroughTaskList() {
    const QString html = MarkdownRenderer::renderToHtml(
        QStringLiteral("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\n~~gone~~\n\n- [x] done\n- [ ] todo"));

    QVERIFY(html.contains(QStringLiteral("<table border=\"0\"")));
    QVERIFY(html.contains(QStringLiteral("<th bgcolor=\"#245995\">A</th>")));
    QVERIFY(html.contains(QStringLiteral("<td bgcolor=\"#ffffff\">1</td>")));
    QVERIFY(html.contains(QStringLiteral("<td bgcolor=\"#f7f7f7\">3</td>")));
    QVERIFY(html.contains(QStringLiteral("<del>gone</del>")));
    QVERIFY(html.contains(QStringLiteral("☑")));
    QVERIFY(html.contains(QStringLiteral("☐")));
    // 复选框不能残留 <input>——QTextDocument 渲染不了。
    QVERIFY(!html.contains(QStringLiteral("<input")));
}

void MarkdownRendererTest::rendersQuoteAccentWithoutDroppingText() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("> 引用里的 **重点**"));
    QVERIFY(html.contains(QStringLiteral("<td width=\"3\" bgcolor=\"#2873c7\"></td>")));
    QVERIFY(html.contains(QStringLiteral("引用里的 <strong>重点</strong>")));
    QVERIFY(!html.contains(QStringLiteral("<blockquote>")));
}

void MarkdownRendererTest::rendersSemanticCallouts_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("accent");
    QTest::newRow("note") << QStringLiteral("NOTE") << QStringLiteral("提示") << QStringLiteral("#1f64b0");
    QTest::newRow("tip") << QStringLiteral("TIP") << QStringLiteral("建议") << QStringLiteral("#16836b");
    QTest::newRow("important") << QStringLiteral("IMPORTANT") << QStringLiteral("重要") << QStringLiteral("#7547a8");
    QTest::newRow("warning") << QStringLiteral("WARNING") << QStringLiteral("注意") << QStringLiteral("#9c640f");
    QTest::newRow("caution") << QStringLiteral("CAUTION") << QStringLiteral("警告") << QStringLiteral("#ba3d40");
}

void MarkdownRendererTest::rendersSemanticCallouts() {
    QFETCH(QString, kind);
    QFETCH(QString, title);
    QFETCH(QString, accent);
    const QString html = MarkdownRenderer::renderToHtml(
        QStringLiteral("> [!%1]\n> **保留正文** 与 `code`\n>\n> 第二段").arg(kind));
    QVERIFY(html.contains(QStringLiteral("<strong>%1</strong>").arg(title)));
    QVERIFY(html.contains(QStringLiteral("<td width=\"3\" bgcolor=\"%1\"></td>").arg(accent)));
    QVERIFY(html.contains(QStringLiteral("<strong>保留正文</strong>")));
    QVERIFY(html.contains(QStringLiteral("第二段")));
    QVERIFY(!html.contains(QStringLiteral("[!%1]").arg(kind)));
    QCOMPARE(html.count(QStringLiteral("<table")), html.count(QStringLiteral("</table>")));
}

void MarkdownRendererTest::preservesLiteralCalloutMarkers() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral(
        "`[!NOTE]`\n\n```text\n> [!WARNING]\n```\n\n> [!UNKNOWN]\n> 普通引用\n\n> [!TIP] 不独占一行"));
    QVERIFY(html.contains(QStringLiteral("[!NOTE]")));
    QVERIFY(html.contains(QStringLiteral("[!WARNING]")));
    QVERIFY(html.contains(QStringLiteral("[!UNKNOWN]")));
    QVERIFY(html.contains(QStringLiteral("[!TIP] 不独占一行")));
}

void MarkdownRendererTest::stylesTaskMarkersAndPreservesCodeLines() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("- [x] done\n- [ ] todo\n\n```cpp\n%1 <literal>\nsecond\n```"));
    QVERIFY(html.contains(QStringLiteral("color:#16836b;\">☑</span>")));
    QVERIFY(html.contains(QStringLiteral("color:#64748b;\">☐</span>")));
    QVERIFY(html.contains(QStringLiteral("%1 &lt;literal&gt;\nsecond")));
}

void MarkdownRendererTest::rendersCalloutsWithMixedLineEndingsAndNestedQuotes() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral(
        "> [!NOTE]\r\n> 第一行\n>\r\n> > 内层引用\r\n>\n> 第三行"));
    QVERIFY(html.contains(QStringLiteral("<strong>提示</strong>")));
    QVERIFY(html.contains(QStringLiteral("第一行")));
    QVERIFY(html.contains(QStringLiteral("内层引用")));
    QVERIFY(html.contains(QStringLiteral("第三行")));
    QVERIFY(!html.contains(QStringLiteral("[!NOTE]")));
    QCOMPARE(html.count(QStringLiteral("<table")), html.count(QStringLiteral("</table>")));
}

void MarkdownRendererTest::toleratesCalloutMarkerCaseAndTrailingSpaces() {
    for (const QString& marker : {QStringLiteral("[!Note] "), QStringLiteral("[!note]\t"), QStringLiteral("[!NOTE]  ")}) {
        const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("> ") + marker + QStringLiteral("\r\n> 正文"));
        QVERIFY(html.contains(QStringLiteral("<strong>提示</strong>")));
        QVERIFY(html.contains(QStringLiteral("正文")));
        QVERIFY(!html.contains(QStringLiteral("[!")));
        QVERIFY(!html.contains(QStringLiteral("<p><br")));
    }
    const QString ordinary = MarkdownRenderer::renderToHtml(QStringLiteral("> [!Note]  同行正文"));
    QVERIFY(ordinary.contains(QStringLiteral("[!Note]  同行正文")));
}

void MarkdownRendererTest::previewHtmlPreservesCodeSemanticsWithoutFontHeuristics() {
    const QString code = MarkdownRenderer::renderPreviewHtml(QStringLiteral("> `[!TIP]`"));
    QVERIFY(code.contains(QStringLiteral("<code>[!TIP]</code>")));
    QVERIFY(!code.contains(QStringLiteral("建议：")));
    const QString quote = MarkdownRenderer::renderPreviewHtml(QStringLiteral("> [!TIP]\n> 正文"));
    QVERIFY(quote.contains(QStringLiteral("建议：正文")));
    QVERIFY(!quote.contains(QStringLiteral("[!TIP]")));
    const QString fenced = MarkdownRenderer::renderPreviewHtml(QStringLiteral("```cpp\n%1 a*b*c\n```"));
    QVERIFY(fenced.contains(QStringLiteral("%1 a*b*c")));
    QVERIFY(!fenced.contains(QStringLiteral(" 行</span>")));
}

void MarkdownRendererTest::previewRejectsOldQtBoundariesInsideEmoji() {
    for (const QString& sequence : {QString::fromUtf8("👨‍👩‍👧‍👦"), QString::fromUtf8("👩🏽‍💻"),
                                    QString::fromUtf8("❤️"), QString::fromUtf8("1️⃣"), QString::fromUtf8("🇨🇳")}) {
        // Simulate every boundary an older Qt could return, even on newer Qt.
        for (int offset = 1; offset < sequence.size(); ++offset) {
            QVERIFY2(PreviewText::continuesEmojiOrMark(sequence, offset),
                     qPrintable(QStringLiteral("unsafe UTF16 offset=%1").arg(offset)));
        }
        QVERIFY(!PreviewText::continuesEmojiOrMark(sequence + sequence, sequence.size()));
        QCOMPARE(PreviewText::truncate(sequence + sequence, 1), sequence + QStringLiteral("…"));
    }
    QCOMPARE(PreviewText::truncate(QStringLiteral("ordinary text"), 4), QStringLiteral("ordi…"));
}

QTEST_MAIN(MarkdownRendererTest)
#include "MarkdownRendererTest.moc"
