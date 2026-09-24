#include <QtTest>

#include "markdown/MarkdownDocument.h"

// 块树的测试。**这一层是纯函数**：一段文本进去、一棵树出来，不碰 QWidget、
// 不需要跑起界面，所以 Markdown 的结构性坑全放在这里测——嵌套列表、
// 代码块里的反引号、软换行、样式叠加。
//
// 绘制那一层（MarkdownView）的 bug 是"看起来不对"，只能靠眼睛；
// 两种混在一起会互相掩盖，所以分开。

namespace {

QString textOf(const QVector<MarkdownSpan>& spans) {
    QString text;
    for (const MarkdownSpan& span : spans) text += span.text;
    return text;
}

}  // namespace

class MarkdownDocumentTest : public QObject {
    Q_OBJECT

private slots:
    void parsesParagraphsAndHeadings();
    void stacksInlineStyles();
    void keepsCodeBlockVerbatim();
    void keepsFencedCodeInsideListItem();
    void flattensNestedLists();
    void readsTaskListsAndOrderedNumbers();
    void dropsUnsafeLinkTargets();
    void foldsSoftBreaksIntoSpaces();
    void keepsSourceForCopying();
    void plainTextIsOneLine();
    void parsesTables();
    void nestsQuotesByDepth();
    void recognizesCallouts();
};

void MarkdownDocumentTest::parsesParagraphsAndHeadings() {
    const MarkdownDocument document =
        MarkdownDocument::parse(QStringLiteral("# 标题\n\n第一段\n\n## 小标题\n\n第二段"));
    const QVector<MarkdownBlock>& blocks = document.blocks();
    QCOMPARE(blocks.size(), 4);

    QCOMPARE(blocks[0].kind, MarkdownBlockKind::Heading);
    QCOMPARE(blocks[0].headingLevel, 1);
    QCOMPARE(textOf(blocks[0].spans), QStringLiteral("标题"));

    QCOMPARE(blocks[1].kind, MarkdownBlockKind::Paragraph);
    QCOMPARE(textOf(blocks[1].spans), QStringLiteral("第一段"));

    QCOMPARE(blocks[2].kind, MarkdownBlockKind::Heading);
    QCOMPARE(blocks[2].headingLevel, 2);
}

void MarkdownDocumentTest::stacksInlineStyles() {
    // 样式会叠加，所以内部用的是位标志而不是枚举。
    // 「加粗的链接」「链接里的行内代码」都是真实会出现的组合。
    const MarkdownDocument document = MarkdownDocument::parse(
        QStringLiteral("普通 **粗体** `代码` ~~删除~~ [**粗链接**](https://example.com)"));
    QCOMPARE(document.blocks().size(), 1);
    const QVector<MarkdownSpan>& spans = document.blocks()[0].spans;

    const auto findStyled = [&spans](MarkdownStyle style) -> MarkdownSpan {
        for (const MarkdownSpan& span : spans) {
            if (span.styles.testFlag(style)) return span;
        }
        return {};
    };

    QCOMPARE(findStyled(MarkdownStyle::Bold).text, QStringLiteral("粗体"));
    QCOMPARE(findStyled(MarkdownStyle::Code).text, QStringLiteral("代码"));
    QCOMPARE(findStyled(MarkdownStyle::Strike).text, QStringLiteral("删除"));

    // 这一条是这个用例存在的理由：粗体**和**链接必须同时成立。
    const MarkdownSpan link = findStyled(MarkdownStyle::Link);
    QCOMPARE(link.text, QStringLiteral("粗链接"));
    QVERIFY(link.styles.testFlag(MarkdownStyle::Bold));
    QCOMPARE(link.href, QStringLiteral("https://example.com"));
}

void MarkdownDocumentTest::keepsCodeBlockVerbatim() {
    // 代码块要一个字符都不改：复制按钮复制的就是它。
    const MarkdownDocument document = MarkdownDocument::parse(
        QStringLiteral("```cpp\nint main() {\n    return **0**;\n}\n```"));
    QCOMPARE(document.blocks().size(), 1);
    const MarkdownBlock& block = document.blocks()[0];
    QCOMPARE(block.kind, MarkdownBlockKind::Code);
    QCOMPARE(block.language, QStringLiteral("cpp"));
    // 里面的 ** 不是粗体，是代码。缩进也要原样保留。
    QCOMPARE(block.code, QStringLiteral("int main() {\n    return **0**;\n}\n"));
}

void MarkdownDocumentTest::keepsFencedCodeInsideListItem() {
    const MarkdownDocument document = MarkdownDocument::parse(QStringLiteral(
        "1. 调用驱动接口：\n\n"
        "   ```cpp\n"
        "   do {\n"
        "       submit();\n"
        "   } while (busy());\n"
        "   ```\n\n"
        "   代码块之后的说明仍属于这个列表项。\n\n"
        "2. 第二项"));

    QCOMPARE(document.blocks().size(), 1);
    const MarkdownBlock& list = document.blocks().first();
    QCOMPARE(list.kind, MarkdownBlockKind::List);
    QCOMPARE(list.items.size(), 2);
    const QString first = textOf(list.items[0].spans);
    QVERIFY(first.contains(QStringLiteral("submit();")));
    QVERIFY(first.contains(QStringLiteral("代码块之后的说明")));
    QVERIFY(list.items[0].spans.constFirst().styles.testFlag(MarkdownStyle::Normal));
    bool hasCode = false;
    for (const MarkdownSpan& span : list.items[0].spans) {
        if (span.styles.testFlag(MarkdownStyle::Code)) hasCode = true;
    }
    QVERIFY(hasCode);
    QCOMPARE(textOf(list.items[1].spans), QStringLiteral("第二项"));
}

void MarkdownDocumentTest::flattensNestedLists() {
    // 嵌套列表**摊平存**，靠 depth 区分。绘制时只需要知道缩进多少，
    // 递归结构反而要在画的时候再展平一次。
    const MarkdownDocument document = MarkdownDocument::parse(
        QStringLiteral("- 一级 A\n  - 二级 A1\n  - 二级 A2\n- 一级 B"));
    QCOMPARE(document.blocks().size(), 1);
    const MarkdownBlock& block = document.blocks()[0];
    QCOMPARE(block.kind, MarkdownBlockKind::List);
    QCOMPARE(block.items.size(), 4);

    QCOMPARE(block.items[0].depth, 0);
    QCOMPARE(textOf(block.items[0].spans), QStringLiteral("一级 A"));
    QCOMPARE(block.items[1].depth, 1);
    QCOMPARE(textOf(block.items[1].spans), QStringLiteral("二级 A1"));
    QCOMPARE(block.items[2].depth, 1);
    QCOMPARE(block.items[3].depth, 0);
}

void MarkdownDocumentTest::readsTaskListsAndOrderedNumbers() {
    const MarkdownDocument tasks =
        MarkdownDocument::parse(QStringLiteral("- [x] 做完了\n- [ ] 还没做"));
    QCOMPARE(tasks.blocks().size(), 1);
    QCOMPARE(tasks.blocks()[0].items.size(), 2);
    QVERIFY(tasks.blocks()[0].items[0].hasCheckbox);
    QVERIFY(tasks.blocks()[0].items[0].checked);
    QVERIFY(tasks.blocks()[0].items[1].hasCheckbox);
    QVERIFY(!tasks.blocks()[0].items[1].checked);

    // 有序列表要把序号算出来——绘制层不该再去数一遍，
    // 而且起始序号可以不是 1（"3. " 开头是合法的）。
    const MarkdownDocument ordered =
        MarkdownDocument::parse(QStringLiteral("3. 第三\n4. 第四\n5. 第五"));
    QCOMPARE(ordered.blocks().size(), 1);
    const QVector<MarkdownListItem>& items = ordered.blocks()[0].items;
    QCOMPARE(items.size(), 3);
    QCOMPARE(items[0].number, 3);
    QCOMPARE(items[1].number, 4);
    QCOMPARE(items[2].number, 5);
    QVERIFY(!items[0].hasCheckbox);
}

void MarkdownDocumentTest::dropsUnsafeLinkTargets() {
    // 协议过滤在**解析阶段**做，不留给绘制层：将来多一个绘制端，那边一忘就漏了。
    // 文字要保留——把整段内容吞掉比留个不能点的链接糟糕得多。
    const MarkdownDocument document = MarkdownDocument::parse(
        QStringLiteral("[好](https://ok.com) [坏](javascript:alert(1)) [锚](#here)"));
    const QVector<MarkdownSpan>& spans = document.blocks()[0].spans;

    QString badText;
    QString badHref;
    for (const MarkdownSpan& span : spans) {
        if (span.text != QStringLiteral("坏")) continue;
        badText = span.text;
        badHref = span.href;
    }
    QCOMPARE(badText, QStringLiteral("坏"));  // 文字还在
    QVERIFY(badHref.isEmpty());               // 目标没了

    bool sawSafe = false;
    bool sawAnchor = false;
    for (const MarkdownSpan& span : spans) {
        if (span.href == QStringLiteral("https://ok.com")) sawSafe = true;
        if (span.href == QStringLiteral("#here")) sawAnchor = true;
    }
    QVERIFY(sawSafe);
    QVERIFY(sawAnchor);
}

void MarkdownDocumentTest::foldsSoftBreaksIntoSpaces() {
    // CommonMark 的行为，和老渲染器、Electron 端一致。两端不一致的话，
    // 同一条消息在手机和桌面上会断在不同位置。
    const MarkdownDocument document =
        MarkdownDocument::parse(QStringLiteral("第一行\n第二行\n\n另一段"));
    QCOMPARE(document.blocks().size(), 2);
    QCOMPARE(textOf(document.blocks()[0].spans), QStringLiteral("第一行 第二行"));
}

void MarkdownDocumentTest::keepsSourceForCopying() {
    // IM 的右键菜单有「复制原始数据」，复制的是 Markdown 原文——
    // 渲染是有损的，标题、代码围栏、链接目标都还原不回来。
    const QString source = QStringLiteral("# 标题\n\n**粗**");
    const MarkdownDocument document = MarkdownDocument::parse(source);
    QCOMPARE(document.source(), source);
}

void MarkdownDocumentTest::plainTextIsOneLine() {
    // 会话列表的预览只有一行：不能带样式标记，也不能带换行——
    // 留着换行的话后面的内容会被截掉，看起来像消息只有半句。
    const MarkdownDocument document = MarkdownDocument::parse(
        QStringLiteral("# 标题\n\n正文 **粗体**\n\n- 一\n- 二\n\n```\n代码\n```"));
    const QString preview = document.plainText();
    QVERIFY(!preview.contains(QChar('\n')));
    QVERIFY(!preview.contains(QStringLiteral("**")));
    QVERIFY(!preview.contains(QChar('#')));
    QVERIFY(preview.contains(QStringLiteral("标题")));
    QVERIFY(preview.contains(QStringLiteral("粗体")));
}

void MarkdownDocumentTest::parsesTables() {
    const MarkdownDocument document = MarkdownDocument::parse(
        QStringLiteral("| 名字 | 值 |\n| --- | --- |\n| a | **1** |\n| b | 2 |"));
    QCOMPARE(document.blocks().size(), 1);
    const MarkdownBlock& block = document.blocks()[0];
    QCOMPARE(block.kind, MarkdownBlockKind::Table);
    QCOMPARE(block.rows.size(), 3);  // 表头 + 两行
    QCOMPARE(block.rows[0].size(), 2);
    QCOMPARE(textOf(block.rows[0][0]), QStringLiteral("名字"));
    QCOMPARE(textOf(block.rows[2][1]), QStringLiteral("2"));
    // 单元格里的行内样式要保住。
    QVERIFY(block.rows[1][1][0].styles.testFlag(MarkdownStyle::Bold));
}

void MarkdownDocumentTest::nestsQuotesByDepth() {
    // 引用是**容器**：里面能装段落、列表、代码。所以它不是「一段带竖条的文字」，
    // 而是给里面的块打一个深度标记——和嵌套列表一样扁平存。
    const MarkdownDocument document = MarkdownDocument::parse(QStringLiteral(
        "正文\n\n> 引用第一段\n>\n> 引用第二段\n>\n> - 引用里的列表\n\n结尾"));
    const QVector<MarkdownBlock>& blocks = document.blocks();

    QCOMPARE(blocks.size(), 5);
    QCOMPARE(blocks[0].quoteDepth(), 0);
    QCOMPARE(textOf(blocks[0].spans), QStringLiteral("正文"));

    QCOMPARE(blocks[1].kind, MarkdownBlockKind::Paragraph);
    QCOMPARE(blocks[1].quoteDepth(), 1);
    QCOMPARE(textOf(blocks[1].spans), QStringLiteral("引用第一段"));
    QCOMPARE(blocks[2].quoteDepth(), 1);
    QCOMPARE(textOf(blocks[2].spans), QStringLiteral("引用第二段"));

    // 引用里的列表还是列表，不能被压成一段文字。
    QCOMPARE(blocks[3].kind, MarkdownBlockKind::List);
    QCOMPARE(blocks[3].quoteDepth(), 1);
    QCOMPARE(blocks[3].items.size(), 1);

    QCOMPARE(blocks[4].quoteDepth(), 0);
    QCOMPARE(textOf(blocks[4].spans), QStringLiteral("结尾"));
}

void MarkdownDocumentTest::recognizesCallouts() {
    // GFM 提示框。老的 HTML 渲染器认这个，换成自绘后不认就是退化。
    const MarkdownDocument tip =
        MarkdownDocument::parse(QStringLiteral("> [!TIP]\n> 记得先备份"));
    QCOMPARE(tip.blocks().size(), 1);
    QCOMPARE(tip.blocks()[0].callout, MarkdownCallout::Tip);
    // 标记本身不能显示出来。
    QCOMPARE(textOf(tip.blocks()[0].spans), QStringLiteral("记得先备份"));

    // 标记和正文写在**同一行**也算。GFM 要求标记独占一行，但老的 HTML 渲染器
    // 一直认这种写法（见 MainWindowLayoutTest 的 same-line-preview-callout），
    // 自绘这边不认就是退化。
    //
    // 这条我一开始写反了：照着 GFM 的严格规则断言「同一行不算」，没去看 IM
    // 那边已经有的用例。以线上行为为准。
    const MarkdownDocument sameLine =
        MarkdownDocument::parse(QStringLiteral("> [!TIP] 记得先备份"));
    QCOMPARE(sameLine.blocks().size(), 1);
    QCOMPARE(sameLine.blocks()[0].callout, MarkdownCallout::Tip);
    QCOMPARE(textOf(sameLine.blocks()[0].spans), QStringLiteral("记得先备份"));

    // 但标记后面必须**隔着空白**。`[!TIP]紧跟正文` 更像有人在写一段以方括号
    // 开头的普通话，照原样留着。
    const MarkdownDocument glued =
        MarkdownDocument::parse(QStringLiteral("> [!TIP]这句话里提到了它"));
    QCOMPARE(glued.blocks()[0].callout, MarkdownCallout::None);
    QVERIFY(textOf(glued.blocks()[0].spans).startsWith(QStringLiteral("[!TIP]")));

    // 不在段首的不算：一句话中间提到 [!TIP] 不该把整段变成提示框。
    const MarkdownDocument midSentence =
        MarkdownDocument::parse(QStringLiteral("> 文档里写的是 [!TIP] 这个写法"));
    QCOMPARE(midSentence.blocks()[0].callout, MarkdownCallout::None);
    QVERIFY(textOf(midSentence.blocks()[0].spans).contains(QStringLiteral("[!TIP]")));

    // 链接和行内代码里的是**字面量**，不是标记。
    const MarkdownDocument linked =
        MarkdownDocument::parse(QStringLiteral("> [[!TIP]](https://example.com) 正文"));
    QCOMPARE(linked.blocks()[0].callout, MarkdownCallout::None);
    QVERIFY(textOf(linked.blocks()[0].spans).startsWith(QStringLiteral("[!TIP]")));
    const MarkdownDocument coded = MarkdownDocument::parse(QStringLiteral("> `[!TIP]` 正文"));
    QCOMPARE(coded.blocks()[0].callout, MarkdownCallout::None);
    QVERIFY(textOf(coded.blocks()[0].spans).startsWith(QStringLiteral("[!TIP]")));

    // 不认识的标记不是提示框。
    const MarkdownDocument unknown =
        MarkdownDocument::parse(QStringLiteral("> [!NOPE]\n> 正文"));
    QCOMPARE(unknown.blocks()[0].callout, MarkdownCallout::None);

    // **相邻的两个引用是两个框。** 深度一样，但不是同一个引用——
    // 只按深度分组的话它们会被合成一个，后面那个的提示框类型直接丢掉。
    const MarkdownDocument twoBoxes = MarkdownDocument::parse(
        QStringLiteral("> [!TIP]\n> 第一个框\n\n> [!WARNING]\n> 第二个框"));
    QCOMPARE(twoBoxes.blocks().size(), 2);
    QCOMPARE(twoBoxes.blocks()[0].callout, MarkdownCallout::Tip);
    QCOMPARE(twoBoxes.blocks()[1].callout, MarkdownCallout::Warning);
    QCOMPARE(twoBoxes.blocks()[0].quoteDepth(), 1);
    QCOMPARE(twoBoxes.blocks()[1].quoteDepth(), 1);
    // 编号必须不同，绘制层就是靠它分框的。
    QVERIFY(twoBoxes.blocks()[0].quoteIds[0] != twoBoxes.blocks()[1].quoteIds[0]);

    // 普通引用没有提示框。
    const MarkdownDocument plain = MarkdownDocument::parse(QStringLiteral("> 就是个引用"));
    QCOMPARE(plain.blocks()[0].callout, MarkdownCallout::None);
    QCOMPARE(plain.blocks()[0].quoteDepth(), 1);
}

QTEST_MAIN(MarkdownDocumentTest)
#include "MarkdownDocumentTest.moc"
