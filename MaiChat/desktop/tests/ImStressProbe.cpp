#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QTest>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
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

    RemoteIMApplication im(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    im.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    im.selectPeer(QStringLiteral("phone-user"));

    QElapsedTimer clock;
    clock.start();
    const auto mark = [&clock](const char* what) {
        qInfo("%6lld ms  %s", clock.elapsed(), what);
        fflush(stdout);
    };

    for (int index = 0; index < count; ++index) {
        im.chatState().receiveText(
            QStringLiteral("phone-user"),
            QStringLiteral("第 %1 条。**加粗**、`行内代码`、[链接](https://example.com)，"
                           "再来一段够长的正文好让它折行：这条消息用来把消息区撑满，"
                           "看看多条一起排版会不会把界面拖住。\n\n"
                           "- 列表第一项\n- 列表第二项\n\n```cpp\nint a = %1;\n```")
                .arg(index));
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

    // 拖窗口宽度：每次宽度变化都会让每条消息重排一次。
    for (const int width : {1100, 900, 1295}) {
        window.resize(width, 857);
        QTest::qWait(300);
        qInfo("%6lld ms  拉到 %d 宽", clock.elapsed(), width);
        fflush(stdout);
    }
    mark("全部完成");
    return 0;
}
