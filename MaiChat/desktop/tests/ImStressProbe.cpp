#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QListWidget>
#include <QTest>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayout.h"
#include "markdown/MarkdownLayoutCache.h"
#include "markdown/MarkdownTheme.h"
#include "ui/MainWindow.h"

// 复现「消息多了界面卡死」：真实会话是几十条历史，视觉探针只有两条。
// 每一步都打时间，卡在哪一步一目了然。
//
//   im_stress_probe [条数]

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    appFont.setHintingPreference(QFont::PreferVerticalHinting);
    app.setFont(appFont);
#endif
    const int count = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 40;
    // 给第二个参数就用**真实账号的本地库**：合成消息再多也不如真数据像。
    // 用 FakeRemoteIMClient 是为了不连网，历史仍然从本地 SQLite 读。
    const QString realUser = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();

    RemoteIMApplication im(realUser.isEmpty() ? QStringLiteral("desktop-user") : realUser,
                           std::make_unique<FakeRemoteIMClient>());
    if (realUser.isEmpty()) {
        im.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
        im.addContact(QStringLiteral("other-user"), QStringLiteral("Mac"));
        im.selectPeer(QStringLiteral("phone-user"));
    }

    // ── 微基准：解析 vs 排版，各占多少 ──────────────────────────
    {
        const QString sample = QStringLiteral(
            "## 小结\n\n**加粗**、`行内代码`、[链接](https://example.com)，再来一段够长的"
            "正文好让它折行，真实的模型回复动辄几千字。\n\n- 一项\n- 二项\n\n"
            "```cpp\nint a = 1;\nfor (int i = 0; i < a; ++i) process(i);\n```\n\n"
            "| 层 | 职责 |\n| --- | --- |\n| 解析 | 文本 → 块树 |\n| 排版 | 块树 → 像素 |\n");
        const MarkdownTheme markdownTheme = MarkdownTheme::standard(1.0);
        constexpr int kRounds = 200;

        QElapsedTimer bench;
        bench.start();
        for (int i = 0; i < kRounds; ++i) {
            const MarkdownDocument parsed = MarkdownDocument::parse(sample);
            Q_UNUSED(parsed);
        }
        const qint64 parseMs = bench.elapsed();

        const MarkdownDocument parsed = MarkdownDocument::parse(sample);
        bench.restart();
        for (int i = 0; i < kRounds; ++i) {
            MarkdownLayout laid;
            laid.layout(parsed, markdownTheme, 700);
        }
        const qint64 layoutMs = bench.elapsed();

        qInfo("       微基准 %d 次：解析 %lld ms，排版 %lld ms（每条 %.2f / %.2f ms）", kRounds,
              parseMs, layoutMs, double(parseMs) / kRounds, double(layoutMs) / kRounds);
        fflush(stdout);
    }

    QElapsedTimer clock;
    clock.start();
    const auto mark = [&clock](const char* what) {
        qInfo("%6lld ms  %s", clock.elapsed(), what);
        fflush(stdout);
    };

    // 纯文本档：条数一样，但内容没有任何 markdown 结构。
    // 和默认档一比，就知道每条 1.65ms 里排版占多少、行部件占多少。
    const bool plain = !qEnvironmentVariableIsEmpty("MAICHAT_PROBE_PLAIN");
    qInfo("       内容档：%s", plain ? "纯文本" : "完整 markdown");
    fflush(stdout);

    for (int index = 0; index < (realUser.isEmpty() ? count : 0); ++index) {
      for (const QString& peer : {QStringLiteral("phone-user"), QStringLiteral("other-user")}) {
        if (plain) {
            im.chatState().receiveText(
                peer, QStringLiteral("第 %1 条，一句白话，没有任何标记。").arg(index));
            continue;
        }
        im.chatState().receiveText(
            peer,
            // 真实的 AICLI 回复是几千字带代码块和表格的，按那个体量造。
            QStringLiteral("## 第 %1 段小结\n\n"
                           "**加粗**、`行内代码`、[链接](https://example.com)，"
                           "再来一段够长的正文好让它折行：这条消息用来把消息区撑满，"
                           "看看多条一起排版会不会把界面拖住。反复这一句把体量堆上去，"
                           "真实的模型回复动辄几千字，一条顶这里十条。\n\n"
                           "- 列表第一项，后面还跟一句解释为什么是这一项\n"
                           "- 列表第二项，同样跟一句，让它长到需要折行\n"
                           "- [ ] 还没做的\n- [x] 做完的\n\n"
                           "```cpp\n// 一段够长的代码块\nint a = %1;\n"
                           "for (int i = 0; i < a; ++i) {\n    process(i);\n}\n```\n\n"
                           "> [!TIP]\n> 提示框也来一个，绘制路径不一样。\n\n"
                           "| 层 | 职责 | 备注 |\n| --- | --- | --- |\n"
                           "| 解析 | 文本 → 块树 | 纯函数 |\n"
                           "| 排版 | 块树 → 像素 | 最贵的一步 |\n"
                           "| 绘制 | 按可见区裁剪 | 便宜 |\n")
                .arg(index));
      }
    }
    mark("历史灌完");

    MainWindow window(im);
    mark("MainWindow 构造完");

    window.resize(1295, 857);
    window.show();
    QTest::qWaitForWindowExposed(&window);
    mark("窗口显示完");

    QTest::qWait(500);
    mark("空转 500ms 之后");

    // **切会话**：用户报的卡死就在这一步。整列消息要拆掉重建。
    QStringList peers;
    if (realUser.isEmpty()) {
        peers << QStringLiteral("other-user") << QStringLiteral("phone-user");
    } else {
        auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
        if (list == nullptr) {
            qWarning("conversationList 没找到");
            return 1;
        }
        for (int row = 0; row < qMin(list->count(), 8); ++row) {
            peers << list->item(row)->data(Qt::UserRole).toString();
        }
        qInfo("       真实会话 %d 个：%s", peers.size(), qPrintable(peers.join(QLatin1Char(' '))));
        fflush(stdout);
    }
    for (int round = 0; round < qMax(6, peers.size() * 2); ++round) {
        const QString peer = peers.at(round % peers.size());
        if (peer.isEmpty()) continue;
        QElapsedTimer one;
        one.start();
        im.selectPeer(peer);
        QTest::qWait(1);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10000);
        qInfo("       切到 %s 用了 %lld ms（缓存 命中 %d / 未命中 %d，存了 %d 份）",
              qPrintable(peer), one.elapsed(), MarkdownLayoutCache::hits(),
              MarkdownLayoutCache::misses(), MarkdownLayoutCache::count());
        fflush(stdout);
    }
    mark("切会话跑完");

    // 拖窗口宽度：每次宽度变化都会让每条消息重排一次。
    for (const int width : {1100, 900, 1295}) {
        QElapsedTimer one;
        one.start();
        window.resize(width, 857);
        QTest::qWait(1);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10000);
        qInfo("       拉到 %d 宽用了 %lld ms", width, one.elapsed());
        fflush(stdout);
    }
    mark("全部完成");
    return 0;
}
