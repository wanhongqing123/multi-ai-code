#include <QApplication>
#include <QFont>
#include <QImage>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "markdown/MarkdownView.h"
#include "ui/MainWindow.h"

// 视觉自检程序：把**真的 MainWindow** 切到 AI 助手页，塞几条内容，存成 PNG。
//
// 为什么不直接跑起 maichat.exe 点一下截图：那条路要登录、要抢前台焦点，
// 合成点击经常落不到窗口上（前台锁），一次验证要试好几轮。这里用和
// MainWindowLayoutTest 同一套办法——FakeRemoteIMClient + 直接构造 MainWindow，
// 不用登录，每次结果一样。
//
// 内容是直接往页面里那个 MarkdownView 塞的，走的是它对外的接口，
// 和真实流式输出最后落到的是同一条路。
//
//   agent_page_visual_probe <输出路径.png>

namespace {

const char* const kAnswer = R"(先说结论：**换成自己绘制之后**，下面这些原来做不到的东西现在都有了。

## 能力

- 代码块有圆角和浅底
- 行内代码是带内边距的胶囊，比如 `MarkdownLayout::paint`
- 任务列表是真的复选框：
  - [x] 块树
  - [x] 排版
  - [ ] 迁 IM

```cpp
// 一篇内容排一次版，多次绘制，按可见区裁剪
layout.paint(&painter, origin, clip);
```

> [!TIP]
> 主题和 IM 共用一份 `MarkdownTheme`，外观要调只改一个地方。

| 层 | 职责 |
| --- | --- |
| MarkdownDocument | 文本 → 块树 |
| MarkdownLayout | 块树 → 像素 |
| MarkdownView | 整个展示区 |
)";

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // 和 main.cpp 设的应用字体保持一致，否则中文落到宋体，截图不代表真机。
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    app.setFont(appFont);
#endif
    const QString output =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("agent-page.png");

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication imApplication(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(imApplication);
    window.resize(1295, 857);
    window.show();
    QTest::qWaitForWindowExposed(&window);

    auto* nav = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
    if (nav == nullptr) {
        qWarning("agentNavButton not found");
        return 1;
    }
    nav->click();
    QTest::qWait(300);

    auto* view = window.findChild<MarkdownView*>();
    if (view == nullptr) {
        qWarning("MarkdownView not found on the agent page");
        return 1;
    }
    view->addItem(QStringLiteral("u1"), MarkdownView::Style::Bubble,
                  QStringLiteral("markdown 现在是自己画的了？给我看看效果"));
    view->addItem(QStringLiteral("a1"), MarkdownView::Style::Document,
                  QString::fromUtf8(kAnswer));
    view->addItem(QStringLiteral("u2"), MarkdownView::Style::Bubble, QStringLiteral("好"));
    QTest::qWait(300);

    QImage image(window.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    window.render(&image);
    if (!image.save(output)) {
        qWarning("failed to save %s", qPrintable(output));
        return 1;
    }
    qInfo("wrote %s (%dx%d)", qPrintable(output), image.width(), image.height());
    return 0;
}
