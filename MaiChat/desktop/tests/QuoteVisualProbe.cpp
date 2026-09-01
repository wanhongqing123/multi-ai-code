// 引用回复的视觉自检：把真实的 MainWindow 跑起来，造几条带引用的消息，
// 截图供人眼确认。单测只能证明「引用块进了布局」，证明不了它长得对不对——
// 之前有过改了两处只落地一处、编译和单测全绿、靠截图才发现的情况。
//
// 这个程序只在本地手工排查时用，不进 ctest。
#include <QApplication>
#include <QTimer>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication application(QStringLiteral("desktop-user"), std::move(client));
    application.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    // 故意让字母序与时间序相反，用来肉眼确认列表按最近消息排而不是按名字排。
    application.addContact(QStringLiteral("aaa-oldest"), QStringLiteral("aaa-oldest"));
    application.addContact(QStringLiteral("zzz-newest"), QStringLiteral("zzz-newest"));
    application.addContact(QStringLiteral("mmm-never"), QStringLiteral("mmm-never"));

    application.selectPeer(QStringLiteral("aaa-oldest"));
    application.sendText(QStringLiteral("我是最早说话的"));
    application.selectPeer(QStringLiteral("zzz-newest"));
    application.sendText(QStringLiteral("我是最新说话的"));

    MainWindow window(application);
    window.show();
    // resize 必须在 show 之后：show 之前设的尺寸会被窗口管理器的初始几何覆盖，
    // 截出来只有一半、消息区被切掉。
    window.resize(1440, 900);

    application.selectPeer(QStringLiteral("phone-user"));

    // 1) 普通消息，作为被引用的原文。
    application.sendText(QStringLiteral("季度报表我放在共享盘了，路径在文档里"));

    // 2) 引用一条文本。
    RemoteIMQuote textQuote;
    textQuote.senderId = QStringLiteral("phone-user");
    textQuote.digest = QStringLiteral("季度报表我放在共享盘了，路径在文档里");
    textQuote.kind = QStringLiteral("text");
    application.sendText(QStringLiteral("收到，我这边同步一下"), textQuote, true);

    // 3) 引用一条很长的文本，检查是否按省略号截断而不是把气泡撑爆。
    RemoteIMQuote longQuote;
    longQuote.senderId = QStringLiteral("phone-user");
    longQuote.digest = QString(160, QChar(0x957F)) + QStringLiteral("…");  // 0x957F = 「长」
    longQuote.kind = QStringLiteral("text");
    application.sendText(QStringLiteral("这条引用很长，检查截断"), longQuote, true);

    // 4) 引用一条图片（没有配文，走类型占位）。
    RemoteIMQuote imageQuote;
    imageQuote.senderId = QStringLiteral("phone-user");
    imageQuote.digest = QStringLiteral("[图片]");
    imageQuote.kind = QStringLiteral("image");
    application.sendText(QStringLiteral("这张图我看过了"), imageQuote, true);

    // 5) 引用一条文件。
    RemoteIMQuote fileQuote;
    fileQuote.senderId = QStringLiteral("phone-user");
    fileQuote.digest = QStringLiteral("[文件] 季度报表.xlsx");
    fileQuote.kind = QStringLiteral("file");
    application.sendText(QStringLiteral("表格我改完了"), fileQuote, true);

    // 直接把部件树渲染成图片，不走屏幕截图：屏幕截图要和窗口管理器、DPI、
    // 遮挡、模态框较劲，而 grab() 只依赖布局本身，结果可复现。
    QCoreApplication::processEvents();
    QTimer::singleShot(600, [&window] {
        QCoreApplication::processEvents();
        const QString out = QStringLiteral(
            "C:/Users/18034/AppData/Local/Temp/claude/E--OpenSource-multi-ai-code/"
            "7f88cf50-b88d-479e-a8fa-caf31bf66c11/scratchpad/quote_grab.png");
        if (window.grab().save(out)) {
            qInfo("saved %s", qPrintable(out));
        } else {
            qWarning("grab failed");
        }
        QCoreApplication::quit();
    });
    return QApplication::exec();
}
