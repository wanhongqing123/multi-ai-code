#include <QCoreApplication>
#include <QTest>
#include <QWidget>

#include "markdown/MarkdownLabel.h"
#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayoutCache.h"

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
    void reusesTheLayoutOfAnIdenticalMessage();
    void neverServesAnotherMessagesLayout();
    void comparesKeysNotJustHashes();
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

void MarkdownLabelTest::reusesTheLayoutOfAnIdenticalMessage() {
    // 切会话会把整列消息拆掉重建，而那些消息一个字都没变。排版是整条链路上最贵
    // 的一步（同一条消息：解析 0.01ms，排版 1.3ms），重排一遍纯属白干——
    // 所以排好的版要留着，下一个部件直接拿。
    //
    // 一个部件手上会有**两份**版：量高度用的和绘制用的。两份都得还回去。
    // Qt 的布局对每条消息至少会按两个不同宽度问高度，量用的那份被覆盖之前
    // 不还回去，等于没有缓存（实测：一次切换白白多排三千次）。
    MarkdownLayoutCache::clear();
    const MarkdownTheme shared = theme();

    {
        MarkdownLabel first;
        first.setTheme(shared);
        first.setMarkdown(longText());
        first.resize(600, 400);
        QCoreApplication::sendPostedEvents(&first, QEvent::Resize);
        // 两个宽度：第二次会把第一次那份量用的版挤掉。
        QVERIFY(first.heightForWidth(300) > 0);
        QVERIFY(first.heightForWidth(450) > 0);
        // 这一下走的是绘制那条路，填的是另一份版。
        QVERIFY(first.contentHeight() > 0);
    }  // 析构：两份都交还给缓存

    const int hitsBefore = MarkdownLayoutCache::hits();
    MarkdownLabel second;
    second.setTheme(shared);
    second.setMarkdown(longText());
    second.resize(600, 400);
    QCoreApplication::sendPostedEvents(&second, QEvent::Resize);
    QVERIFY(second.heightForWidth(300) > 0);
    QVERIFY(second.heightForWidth(450) > 0);
    QVERIFY(second.contentHeight() > 0);

    // 三个宽度一次都不该重排。
    QCOMPARE(MarkdownLayoutCache::hits() - hitsBefore, 3);
}

void MarkdownLabelTest::neverServesAnotherMessagesLayout() {
    // 内容、宽度、附注、皮肤，任意一样不同就是另一份版。
    MarkdownLayoutCache::clear();
    const MarkdownTheme shared = theme();

    MarkdownLabel warm;
    warm.setTheme(shared);
    warm.setMarkdown(longText());
    const int tall = warm.heightForWidth(300);
    const int wide = warm.heightForWidth(700);
    QVERIFY(tall > wide);

    MarkdownLabel other;
    other.setTheme(shared);
    other.setMarkdown(QStringLiteral("短"));
    QVERIFY2(other.heightForWidth(300) < tall, "拿到了别人的版");

    MarkdownLabel same;
    same.setTheme(shared);
    same.setMarkdown(longText());
    QCOMPARE(same.heightForWidth(300), tall);
    QCOMPARE(same.heightForWidth(700), wide);

    MarkdownLabel noted;
    noted.setTheme(shared);
    noted.setTrailingNote(QStringLiteral("  · 16:13"), QColor(QStringLiteral("#0f8ddd")), 11);
    noted.setMarkdown(longText());
    QVERIFY(noted.heightForWidth(300) >= tall);

    MarkdownLabel zoomed;
    zoomed.setTheme(MarkdownTheme::standard(2.0));
    zoomed.setMarkdown(longText());
    QVERIFY2(zoomed.heightForWidth(300) > tall, "换了皮肤却拿到了旧皮肤的版");
}

void MarkdownLabelTest::comparesKeysNotJustHashes() {
    // 缓存按哈希找桶，**哈希会撞**。撞上一次就是把别人的消息画到这条上，
    // 那种 bug 找起来要命，所以命中之后还要把键逐项比一遍。
    //
    // 部件那一层撞不出来（它只给整数宽度），所以直接对着缓存测：
    // 宽度取整到 1/16 之后 600.0 和 600.01 落进同一个桶，但它们不是同一个键。
    MarkdownLayoutCache::clear();
    const MarkdownTheme shared = theme();
    const QString source = longText();

    auto laid = std::make_unique<MarkdownLayout>();
    laid->layout(MarkdownDocument::parse(source), shared, 600.0);
    MarkdownLayoutCache::put(source, QString(), 0, 0, 600.0, shared.zoom, std::move(laid));

    QVERIFY2(MarkdownLayoutCache::take(source, QString(), 0, 0, 600.01, shared.zoom) == nullptr,
             "宽度不一样却把那份给出去了——说明只比了哈希没比键");
    QVERIFY2(MarkdownLayoutCache::take(source, QString(), 0, 0, 600.0, shared.zoom) != nullptr,
             "宽度一样反而没给");
}

QTEST_MAIN(MarkdownLabelTest)
#include "MarkdownLabelTest.moc"
