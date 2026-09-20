#include <QLabel>
#include <QScrollBar>
#include <QSignalSpy>
#include <QtTest>

#include "markdown/MarkdownView.h"

// 消息展示区的测试。
//
// 这里守的是**换掉「一条消息一个部件」之后新拿到的那些能力**：跨消息选区、
// 短气泡按内容收窄、贴底跟随、嵌进来的部件跟着滚。这几条都是回归起来最不容易
// 被发现的——看一眼界面「好像没问题」，实际已经坏了。

namespace {

MarkdownTheme testTheme() {
    return MarkdownTheme::standard(1.0);
}

}  // namespace

class MarkdownViewTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void selectsAcrossMessages();
    void shortBubbleIsNarrowerThanLongOne();
    void followsBottomUntilTheUserScrollsUp();
    void embeddedWidgetMovesWithTheContent();
    void updatingOneItemMovesTheOnesAfterIt();
    void emitsLinkActivatedOnClick();
    void clearDropsEverything();
    void readingColumnStaysCenteredAndCapped();
    void embeddedWidgetGrowingPushesTheRestDown();

private:
    void prepare(MarkdownView& view);
};

void MarkdownViewTest::initTestCase() {
    // 字体量不出来的话所有几何断言都没意义，先确认平台插件带字体。
    QVERIFY(!QGuiApplication::platformName().isEmpty());
}

void MarkdownViewTest::prepare(MarkdownView& view) {
    view.setTheme(testTheme());
    view.resize(520, 260);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
}

void MarkdownViewTest::selectsAcrossMessages() {
    // 这是这次重写最主要的收益。原来每条消息各是一个 QTextBrowser，
    // 选区过不了界，想连着复制两条只能分两次。
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Bubble, QStringLiteral("第一条"));
    view.addItem(QStringLiteral("b"), MarkdownView::Style::Document, QStringLiteral("第二条"));
    view.addItem(QStringLiteral("c"), MarkdownView::Style::Document, QStringLiteral("第三条"));

    view.selectAll();
    const QString selected = view.selectedText();
    QVERIFY(selected.contains(QStringLiteral("第一条")));
    QVERIFY(selected.contains(QStringLiteral("第二条")));
    QVERIFY(selected.contains(QStringLiteral("第三条")));
    // 消息之间要断开，复制出来不能糊成一行。
    QVERIFY(selected.contains(QLatin1Char('\n')));

    view.clearSelection();
    QVERIFY(view.selectedText().isEmpty());
}

void MarkdownViewTest::shortBubbleIsNarrowerThanLongOne() {
    // 气泡按内容收窄。「好」和一整段话占一样宽的话，界面会很空。
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("short"), MarkdownView::Style::Bubble, QStringLiteral("好"));
    view.addItem(QStringLiteral("long"), MarkdownView::Style::Bubble,
                 QStringLiteral("这是一条长得多的消息，长到需要占满允许的最大宽度还要折行"));

    const QRectF shortRect = view.itemRect(QStringLiteral("short"));
    const QRectF longRect = view.itemRect(QStringLiteral("long"));
    QVERIFY(shortRect.width() > 0);
    QVERIFY(shortRect.width() < longRect.width());

    // 两个都贴右边。气泡是「我发的」，靠右是这个语义的全部表达。
    QVERIFY(qAbs(shortRect.right() - longRect.right()) < 1.5);

    // 再长也不能占满整行。
    QVERIFY(longRect.width() < view.viewport()->width());
}

void MarkdownViewTest::followsBottomUntilTheUserScrollsUp() {
    MarkdownView view;
    prepare(view);
    for (int index = 0; index < 20; ++index) {
        view.addItem(QStringLiteral("m%1").arg(index), MarkdownView::Style::Document,
                     QStringLiteral("第 %1 条消息").arg(index));
    }
    QVERIFY(view.contentHeight() > view.viewport()->height());
    // 内容在长的时候默认贴底，新消息才看得见。
    QVERIFY(view.isAtBottom());

    // 用户往上翻之后就不许再被拽回去——正在看历史时被拉到底是很烦的。
    view.verticalScrollBar()->setValue(0);
    QVERIFY(!view.isAtBottom());
    view.addItem(QStringLiteral("new"), MarkdownView::Style::Document, QStringLiteral("又一条"));
    QVERIFY(!view.isAtBottom());

    view.scrollToBottom();
    QVERIFY(view.isAtBottom());
}

void MarkdownViewTest::embeddedWidgetMovesWithTheContent() {
    // 工具卡那种带按钮的东西画不出来，得真嵌一个部件进去。
    // 视图要负责摆位置、跟着滚、滚出去藏起来。
    MarkdownView view;
    prepare(view);
    for (int index = 0; index < 12; ++index) {
        view.addItem(QStringLiteral("m%1").arg(index), MarkdownView::Style::Document,
                     QStringLiteral("第 %1 条消息").arg(index));
    }
    auto* card = new QLabel(QStringLiteral("工具卡"));
    card->setFixedHeight(40);
    view.addWidget(QStringLiteral("card"), card);

    QCOMPARE(card->parentWidget(), view.viewport());
    view.scrollToBottom();
    QVERIFY(card->isVisible());

    // 位置必须真的按内容坐标减滚动量算出来，不能只是「碰巧可见」。
    const QRectF slot = view.itemRect(QStringLiteral("card"));
    QVERIFY(slot.height() > 0);
    const int offset = view.verticalScrollBar()->value();
    QCOMPARE(card->y(), qRound(slot.top()) - offset);
    QCOMPARE(card->height(), qRound(slot.height()));

    // 滚到顶上之后卡片在很远的下面，应该被藏起来而不是糊在别的内容上。
    view.verticalScrollBar()->setValue(0);
    QVERIFY(slot.top() > view.viewport()->height());
    QVERIFY(!card->isVisible());

    // 再滚回去还得回到原位。
    view.scrollToBottom();
    QVERIFY(card->isVisible());
    QCOMPARE(card->y(), qRound(slot.top()) - view.verticalScrollBar()->value());
}

void MarkdownViewTest::updatingOneItemMovesTheOnesAfterIt() {
    // 流式输出就是这个形状：中间那条一直在长，后面的要跟着往下挪。
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document, QStringLiteral("开头"));
    view.addItem(QStringLiteral("b"), MarkdownView::Style::Document, QStringLiteral("短"));
    view.addItem(QStringLiteral("c"), MarkdownView::Style::Document, QStringLiteral("结尾"));

    const QRectF beforeA = view.itemRect(QStringLiteral("a"));
    const qreal beforeC = view.itemRect(QStringLiteral("c")).top();

    view.updateItem(QStringLiteral("b"),
                    QStringLiteral("变长了很多很多的一段内容，长到一定会折成好几行，"
                                   "这样后面那条的位置必然要往下挪。再补一句让它更长一点。"));

    // 前面那条不该动。
    QCOMPARE(view.itemRect(QStringLiteral("a")), beforeA);
    // 后面那条要往下挪。
    QVERIFY(view.itemRect(QStringLiteral("c")).top() > beforeC);

    QVERIFY(view.contains(QStringLiteral("b")));
    QCOMPARE(view.itemCount(), 3);
}

void MarkdownViewTest::emitsLinkActivatedOnClick() {
    MarkdownView view;
    prepare(view);
    // **链接放在行尾**，这一条才测得到东西：xToCursor 会把行尾右边的空白
    // 一律映射到最后一个字符上，链接如果在行尾，少了行宽判断的话
    // 整行右边一大片空白都会变成可点的。链接放行首的话这条断言是白写的。
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document,
                 QStringLiteral("前面是普通文字 [点我](https://example.com)"));

    QSignalSpy spy(&view, &MarkdownView::linkActivated);
    const QRectF rect = view.itemRect(QStringLiteral("a"));
    const int row = static_cast<int>(rect.top()) + 8;

    // 先确认这条内容确实没占满整行，右边真的有空白可点。
    QVERIFY(view.itemRect(QStringLiteral("a")).width() > 200);

    // 行尾右边的空白不是链接。
    const QPoint blank(static_cast<int>(rect.right()) - 6, row);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), blank);
    QCOMPARE(spy.count(), 0);

    // 链接本身要点得中。文字末尾往左找一点，落在「点我」上。
    bool activated = false;
    for (int x = static_cast<int>(rect.left()) + 40; x < static_cast<int>(rect.right()); x += 4) {
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(x, row));
        if (spy.count() > 0) {
            activated = true;
            break;
        }
    }
    QVERIFY(activated);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("https://example.com"));
}

void MarkdownViewTest::clearDropsEverything() {
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document, QStringLiteral("内容"));
    view.addWidget(QStringLiteral("card"), new QLabel(QStringLiteral("卡片")));
    QCOMPARE(view.itemCount(), 2);

    view.clear();
    QVERIFY(view.isEmpty());
    QCOMPARE(view.itemCount(), 0);
    QVERIFY(view.selectedText().isEmpty());
    QVERIFY(!view.contains(QStringLiteral("a")));
}

void MarkdownViewTest::readingColumnStaysCenteredAndCapped() {
    // 窗口很宽时正文不能跟着拉宽：一行太长，眼睛回扫会丢行。
    // 超出的部分留白，内容**居中**。
    MarkdownView view;
    prepare(view);
    view.setMaxContentWidth(300);
    view.resize(900, 260);
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document,
                 QStringLiteral("一段会被限制在阅读列宽度之内的正文"));

    const QRectF rect = view.itemRect(QStringLiteral("a"));
    QCOMPARE(qRound(rect.width()), 300);
    // 两边留白要一样宽。原来是靠布局权重摆的，权重给错一次列就只剩三分之一宽。
    const qreal leftGap = rect.left();
    const qreal rightGap = view.viewport()->width() - rect.right();
    QVERIFY(qAbs(leftGap - rightGap) < 1.5);

    // 窗口比列还窄时就该占满，不能反过来溢出去。
    view.resize(220, 260);
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QRectF narrow = view.itemRect(QStringLiteral("a"));
    QVERIFY(narrow.width() < 300);
    QVERIFY(narrow.right() <= view.viewport()->width());
}

void MarkdownViewTest::embeddedWidgetGrowingPushesTheRestDown() {
    // 工具卡展开、思考条收起之后高度变了，后面所有东西都得往下挪。
    // 不跟的话卡片会盖住下一条消息。
    MarkdownView view;
    prepare(view);
    auto* card = new QLabel(QStringLiteral("工具卡"));
    card->setFixedHeight(30);
    view.addWidget(QStringLiteral("card"), card);
    view.addItem(QStringLiteral("after"), MarkdownView::Style::Document,
                 QStringLiteral("卡片后面的一条"));

    const qreal before = view.itemRect(QStringLiteral("after")).top();
    QCOMPARE(qRound(view.itemRect(QStringLiteral("card")).height()), 30);

    card->setFixedHeight(120);
    QCOMPARE(qRound(view.itemRect(QStringLiteral("card")).height()), 120);
    QCOMPARE(view.itemRect(QStringLiteral("after")).top(), before + 90);
}

QTEST_MAIN(MarkdownViewTest)
#include "MarkdownViewTest.moc"
