#include <QtTest>

#include "model/MessageSearch.h"

namespace {

RemoteIMMessage textMessage(const QString& id, const QString& text) {
    RemoteIMMessage message;
    message.id = id;
    message.text = text;
    return message;
}

QList<RemoteIMMessage> sampleConversation() {
    return {
        textMessage(QStringLiteral("m0"), QStringLiteral("早上好")),
        textMessage(QStringLiteral("m1"), QStringLiteral("构建失败了，看下 CMake")),
        textMessage(QStringLiteral("m2"), QStringLiteral("[图片消息] build-log.png")),
        textMessage(QStringLiteral("m3"), QStringLiteral("cmake 已经修好")),
        textMessage(QStringLiteral("m4"), QStringLiteral("收到")),
    };
}

}  // namespace

class MessageSearchTest : public QObject {
    Q_OBJECT

private slots:
    void findsMatchesInTimeOrder();
    void matchIsCaseInsensitive();
    void matchesAttachmentPlaceholderText();
    void blankQueryMatchesNothing();
    void nextWrapsAroundToFirst();
    void previousWrapsAroundToLast();
    void navigationOnEmptyHitsReturnsInvalid();
    void navigationFromUnknownPositionPicksNearestEnd();
    void scoresExactMatchesAboveFuzzyOnes();
    void matchesWhenWordsAreRememberedOutOfOrder();
    void matchesWhenAFewCharactersAreMisremembered();
    void singleCharacterQueryDoesNotMatchEverything();
    void subsequenceRejectsMatchesScatteredAcrossLongText();
    void subsequenceStillMatchesWhenTheTightHitComesAfterAScatteredOne();
};

void MessageSearchTest::findsMatchesInTimeOrder() {
    const QList<int> hits =
        MessageSearch::matchIndexes(sampleConversation(), QStringLiteral("CMake"));
    QCOMPARE(hits, QList<int>({1, 3}));
}

void MessageSearchTest::matchIsCaseInsensitive() {
    const QList<int> upper =
        MessageSearch::matchIndexes(sampleConversation(), QStringLiteral("CMAKE"));
    const QList<int> lower =
        MessageSearch::matchIndexes(sampleConversation(), QStringLiteral("cmake"));
    QCOMPARE(upper, QList<int>({1, 3}));
    QCOMPARE(lower, upper);
}

// 图片/文件消息的正文是「[图片消息] 文件名」这类占位文本，按文件名要能搜到。
void MessageSearchTest::matchesAttachmentPlaceholderText() {
    const QList<int> hits =
        MessageSearch::matchIndexes(sampleConversation(), QStringLiteral("build-log"));
    QCOMPARE(hits, QList<int>({2}));
}

// 空查询不能把整个会话都算成命中，否则清空输入框会「全部高亮」。
void MessageSearchTest::blankQueryMatchesNothing() {
    QVERIFY(MessageSearch::matchIndexes(sampleConversation(), QString()).isEmpty());
    QVERIFY(MessageSearch::matchIndexes(sampleConversation(), QStringLiteral("   ")).isEmpty());
}

void MessageSearchTest::nextWrapsAroundToFirst() {
    const QList<int> hits{1, 3};
    QCOMPARE(MessageSearch::nextHit(hits, 1), 3);
    QCOMPARE(MessageSearch::nextHit(hits, 3), 1);
}

void MessageSearchTest::previousWrapsAroundToLast() {
    const QList<int> hits{1, 3};
    QCOMPARE(MessageSearch::previousHit(hits, 3), 1);
    QCOMPARE(MessageSearch::previousHit(hits, 1), 3);
}

void MessageSearchTest::navigationOnEmptyHitsReturnsInvalid() {
    QCOMPARE(MessageSearch::nextHit({}, 0), -1);
    QCOMPARE(MessageSearch::previousHit({}, 0), -1);
}

// 改关键词后旧的当前位置可能不再是命中，此时向后取第一个、向前取最后一个，
// 不能返回 -1 让界面显示「没有结果」。
void MessageSearchTest::navigationFromUnknownPositionPicksNearestEnd() {
    const QList<int> hits{2, 5, 9};
    QCOMPARE(MessageSearch::nextHit(hits, -1), 2);
    QCOMPARE(MessageSearch::previousHit(hits, 100), 9);
}


// 分级的意义：原样命中要排在模糊命中前面，否则「记岔一点也能搜到」会把
// 真正想找的那条挤下去。
void MessageSearchTest::scoresExactMatchesAboveFuzzyOnes() {
    const int prefix = MessageSearch::score(QStringLiteral("cmake 失败了"), QStringLiteral("cmake"));
    const int middle = MessageSearch::score(QStringLiteral("构建时 cmake 失败"), QStringLiteral("cmake"));
    const int fuzzy = MessageSearch::score(QStringLiteral("c 开头 m 中间 ake 结尾"), QStringLiteral("cmake"));
    QCOMPARE(prefix, static_cast<int>(MessageSearch::Prefix));
    QCOMPARE(middle, static_cast<int>(MessageSearch::Substring));
    QCOMPARE(fuzzy, static_cast<int>(MessageSearch::Subsequence));
    QVERIFY(prefix > middle);
    QVERIFY(middle > fuzzy);
}

// 词序记反也要能搜到：严格子串在这里会一无所获。
void MessageSearchTest::matchesWhenWordsAreRememberedOutOfOrder() {
    const QString text = QStringLiteral("cmake 那一步失败了");
    QCOMPARE(MessageSearch::score(text, QStringLiteral("失败 cmake")),
             static_cast<int>(MessageSearch::AllTokens));
    QVERIFY(MessageSearch::matches(text, QStringLiteral("失败 cmake")));
}

// 中间漏字也要能搜到：「构建失败」找得到「构建那一步失败了」。
void MessageSearchTest::matchesWhenAFewCharactersAreMisremembered() {
    QVERIFY(MessageSearch::matches(QStringLiteral("构建那一步失败了"), QStringLiteral("构建失败")));
    QVERIFY(!MessageSearch::matches(QStringLiteral("构建那一步失败了"), QStringLiteral("失败构建")));
}

// 但模糊不能糊到没用：单字查询若走子序列，几乎所有消息都会命中。
void MessageSearchTest::singleCharacterQueryDoesNotMatchEverything() {
    QCOMPARE(MessageSearch::score(QStringLiteral("今天的构建通过了"), QStringLiteral("天")),
             static_cast<int>(MessageSearch::Substring));
    QCOMPARE(MessageSearch::score(QStringLiteral("今日构建通过"), QStringLiteral("天")),
             static_cast<int>(MessageSearch::NoMatch));
}

void MessageSearchTest::subsequenceRejectsMatchesScatteredAcrossLongText() {
    // 真实记录里最极端的那种：一千多字的文档，四个字按顺序散落在首尾。
    // 这类命中没有意义，而结果面板只有 60 个位置，它会把贴切的挤出去。
    QString scattered = QStringLiteral("构");
    scattered += QString(400, QLatin1Char('x'));
    scattered += QStringLiteral("建");
    scattered += QString(400, QLatin1Char('y'));
    scattered += QStringLiteral("失");
    scattered += QString(400, QLatin1Char('z'));
    scattered += QStringLiteral("败");
    QCOMPARE(MessageSearch::score(scattered, QStringLiteral("构建失败")),
             static_cast<int>(MessageSearch::NoMatch));

    // 紧凑的那种必须继续命中：4 个字的查询，窗口上限 16。
    QCOMPARE(MessageSearch::score(QStringLiteral("构建那一步失败了"), QStringLiteral("构建失败")),
             static_cast<int>(MessageSearch::Subsequence));
}

void MessageSearchTest::subsequenceStillMatchesWhenTheTightHitComesAfterAScatteredOne() {
    // 这条专门守住「换起点重试」这一步，而不是「反向收紧起点」那一步——两者容易混。
    //
    // 构造：开头就有一处完整但过散的匹配（构、建 挨着，失、败 在 50 个字之后），
    // 它的最小窗口仍然超限；真正紧凑的那处在正文后半段。
    // 前向贪心第一次落到的就是开头那处，反向收紧也救不回来（起点就是 0），
    // 必须从收紧后的起点之后重新找，才能发现后面那处。
    QString text = QStringLiteral("构建");
    text += QString(50, QLatin1Char('x'));
    text += QStringLiteral("失败");
    text += QString(200, QLatin1Char('v'));
    text += QStringLiteral("构建那一步失败了");
    QCOMPARE(MessageSearch::score(text, QStringLiteral("构建失败")),
             static_cast<int>(MessageSearch::Subsequence));
}

QTEST_MAIN(MessageSearchTest)
#include "MessageSearchTest.moc"
