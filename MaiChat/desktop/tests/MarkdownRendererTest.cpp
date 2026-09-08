#include <QTest>

#include "markdown/MarkdownRenderer.h"

class MarkdownRendererTest : public QObject {
    Q_OBJECT

private slots:
    void rendersCommonAiCliMarkdown();
    void collapsesSoftLineBreaksLikeElectron();
    void escapesRawHtmlAndUnsafeLinks();
    void rendersFencedCodeWithoutInlineFormatting();
    void rendersGfmTableStrikethroughTaskList();
    void rendersCodeCardLabels_data();
    void rendersCodeCardLabels();
    void rendersQuoteAccentWithoutDroppingText();
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

    QVERIFY(html.contains(QStringLiteral("<table border=\"1\"")));
    QVERIFY(html.contains(QStringLiteral("<th bgcolor=\"#e8f0f8\">A</th>")));
    QVERIFY(html.contains(QStringLiteral("<td bgcolor=\"#ffffff\">1</td>")));
    QVERIFY(html.contains(QStringLiteral("<td bgcolor=\"#f6f8fb\">3</td>")));
    QVERIFY(html.contains(QStringLiteral("<del>gone</del>")));
    QVERIFY(html.contains(QStringLiteral("☑")));
    QVERIFY(html.contains(QStringLiteral("☐")));
    // 复选框不能残留 <input>——QTextDocument 渲染不了。
    QVERIFY(!html.contains(QStringLiteral("<input")));
}

void MarkdownRendererTest::rendersCodeCardLabels_data() {
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("label");
    QTest::newRow("named-language") << QStringLiteral("cpp") << QStringLiteral("cpp");
    QTest::newRow("fallback-label") << QString() << QStringLiteral("代码");
}

void MarkdownRendererTest::rendersCodeCardLabels() {
    QFETCH(QString, language);
    QFETCH(QString, label);
    const QString html = MarkdownRenderer::renderToHtml(
        QStringLiteral("```") + language + QStringLiteral("\n%1 <literal>\n```"));
    // Match the visible label, not the language-cpp attribute on the code element.
    QVERIFY(html.contains(QLatin1Char('>') + label + QStringLiteral("</span>")));
    QVERIFY(html.contains(QStringLiteral("%1 &lt;literal&gt;")));
}

void MarkdownRendererTest::rendersQuoteAccentWithoutDroppingText() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("> 引用里的 **重点**"));
    QVERIFY(html.contains(QStringLiteral("<td width=\"3\" bgcolor=\"#90c9ed\"></td>")));
    QVERIFY(html.contains(QStringLiteral("引用里的 <strong>重点</strong>")));
    QVERIFY(!html.contains(QStringLiteral("<blockquote>")));
}

QTEST_MAIN(MarkdownRendererTest)
#include "MarkdownRendererTest.moc"
