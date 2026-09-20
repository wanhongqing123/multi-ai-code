#include <QCoreApplication>
#include <QTest>
#include <QWidget>

#include "markdown/MarkdownLabel.h"

// MarkdownLabel：不滚动、高度跟着内容走的那一半（IM 的消息气泡用它）。
//
// 这里盯的是**宽度契约**。它有两个入口会排版：heightForWidth（布局来问高度）
// 和 resizeEvent（自己变宽了）。两个宽度可能不一样，一旦搞混，画出来就是
// 按错的宽度折行——右边留一大片空白，或者一句话被切成好几行。

class MarkdownLabelTest : public QObject {
    Q_OBJECT

private slots:
    void reportsTallerWhenNarrower();
    void measuringAnotherWidthDoesNotStickToIt();
    void trailingNoteStaysOnTheLastLine();
    void plainTextDropsMarkup();
};

namespace {

MarkdownTheme theme() {
    return MarkdownTheme::standard(1.0);
}

// 一段足够长、一定会折行的正文。
QString longText() {
    return QStringLiteral(
        "这是一段足够长的正文，用来确保在窄一点的宽度下一定会折行，"
        "这样「按哪个宽度排的版」才有观测得到的后果。");
}

}  // namespace

void MarkdownLabelTest::reportsTallerWhenNarrower() {
    MarkdownLabel label;
    label.setTheme(theme());
    label.setMarkdown(longText());
    QVERIFY(label.heightForWidth(200) > label.heightForWidth(600));
}

void MarkdownLabelTest::measuringAnotherWidthDoesNotStickToIt() {
    // Qt 的布局在定案之前会拿好几个试探宽度来问高度。**问完之后状态必须回到
    // 自己的宽度上**——沿用最后一次测量的那个宽度，画出来就是按它折行：
    // 一句话被切成好几行，右边一大片空白。
    //
    // 这里断的是对外的两个入口（contentHeight / sizeHint）。绘制那条路上是同
    // 一句 ensureLayout，但用例卡不住它：顶层部件 render() 会先建原生窗口，
    // 那一步自己就会重排一次，怎么写都是绿的（试过，是条空断言）。
    MarkdownLabel label;
    label.setTheme(theme());
    label.setMarkdown(longText());
    label.resize(600, 400);
    // resize() 只是把事件排进队列，先冲掉；不然后面随便一个动作把它带出来，
    // 版就被重新按 600 排了一遍，这个用例也就测不到东西了。
    QCoreApplication::sendPostedEvents(&label, QEvent::Resize);

    const int wide = label.contentHeight();
    QVERIFY(wide > 0);

    const int narrow = label.heightForWidth(120);
    QVERIFY2(narrow > wide, "窄宽度应当排得更高，不然这个用例没在测东西");

    // 部件自己还是 600 宽。
    QCOMPARE(label.contentHeight(), wide);
    QCOMPARE(label.sizeHint().height(), wide);
}

void MarkdownLabelTest::trailingNoteStaysOnTheLastLine() {
    MarkdownLabel label;
    label.setTheme(theme());
    label.setTrailingNote(QStringLiteral("  · 16:13"), QColor(QStringLiteral("#0f8ddd")), 11);
    label.setMarkdown(QStringLiteral("好"));
    label.resize(600, 100);

    // 接在正文末尾，不另起一行——块之间会补换行，另起一行这里就会有换行。
    QVERIFY(label.plainText().endsWith(QStringLiteral("16:13")));
    QVERIFY(!label.plainText().contains(QLatin1Char('\n')));

    // 而且不该比没有附注时高出一整行。
    MarkdownLabel bare;
    bare.setTheme(theme());
    bare.setMarkdown(QStringLiteral("好"));
    QCOMPARE(label.heightForWidth(600), bare.heightForWidth(600));
}

void MarkdownLabelTest::plainTextDropsMarkup() {
    MarkdownLabel label;
    label.setTheme(theme());
    label.setMarkdown(QStringLiteral("# 标题\n\n**加粗**与 `代码`"));
    QVERIFY(!label.plainText().contains(QStringLiteral("# 标题")));
    QVERIFY(!label.plainText().contains(QStringLiteral("**")));
    QVERIFY(label.plainText().contains(QStringLiteral("加粗")));
    // 原文要原样留着：「复制原始数据」复制的是它。
    QCOMPARE(label.markdown(), QStringLiteral("# 标题\n\n**加粗**与 `代码`"));
}

QTEST_MAIN(MarkdownLabelTest)
#include "MarkdownLabelTest.moc"
