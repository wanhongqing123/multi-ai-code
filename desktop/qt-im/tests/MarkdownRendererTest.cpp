#include <QTest>

#include "markdown/MarkdownRenderer.h"

class MarkdownRendererTest : public QObject {
    Q_OBJECT

private slots:
    void rendersCommonAiCliMarkdown();
    void preservesSingleLineBreaksInParagraphs();
    void escapesRawHtmlAndUnsafeLinks();
    void rendersFencedCodeWithoutInlineFormatting();
};

void MarkdownRendererTest::rendersCommonAiCliMarkdown() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("## 标题\n\n**重点** 和 `code`\n\n- 第一条\n- [链接](https://example.com)"));

    QVERIFY(html.contains(QStringLiteral("<h2>标题</h2>")));
    QVERIFY(html.contains(QStringLiteral("<strong>重点</strong>")));
    QVERIFY(html.contains(QStringLiteral("<code>code</code>")));
    QVERIFY(html.contains(QStringLiteral("<ul>")));
    QVERIFY(html.contains(QStringLiteral("<li>第一条</li>")));
    QVERIFY(html.contains(QStringLiteral("href=\"https://example.com\"")));
}

void MarkdownRendererTest::preservesSingleLineBreaksInParagraphs() {
    const QString html = MarkdownRenderer::renderToHtml(QStringLiteral("line 1\nline 2"));

    QVERIFY(html.contains(QStringLiteral("line 1<br/>line 2")));
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

    QVERIFY(html.contains(QStringLiteral("<pre><code>**not bold** &lt;tag&gt;")));
    QVERIFY(!html.contains(QStringLiteral("<strong>not bold</strong>")));
}

QTEST_MAIN(MarkdownRendererTest)
#include "MarkdownRendererTest.moc"
