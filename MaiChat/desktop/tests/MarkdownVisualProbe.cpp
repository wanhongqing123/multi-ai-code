#include <QApplication>
#include <QFont>
#include <QImage>
#include <QPainter>

#include "markdown/MarkdownView.h"

// 视觉自检程序：把一份覆盖各种语法的 Markdown 画出来存成 PNG，手工看。
// 不进 ctest——「好不好看」没法写断言，但**必须真看一眼**，
// 编译过和测试绿都证明不了排版好看。
//
//   markdown_visual_probe <输出路径.png>

namespace {

const char* const kSample = R"(# 自己绘制的 Markdown

这是一段正文，里面有 **加粗**、*强调*、`行内代码`、~~删除线~~ 和一个
[链接](https://example.com)。行内代码现在有内边距和圆角的胶囊底——
这是 QTextDocument 的 CSS 子集画不出来的东西之一。

## 列表

- 一级项目，写长一点看看折行之后的缩进对不对，第二行应该和第一行的文字对齐
  - 二级项目
  - 另一个二级项目
- 回到一级

1. 有序列表
2. 序号右对齐贴着正文
3. 第三项

- [x] 做完的任务
- [ ] 还没做的任务

## 代码块

```cpp
int main() {
    // 圆角、浅底、等宽字体

    return 0;
}
```

## 引用和提示框

> 普通引用。里面可以有 **样式**，也可以有好几段。
>
> 第二段在同一个框里。

> [!TIP]
> 提示框有自己的颜色和中文标题。

> [!WARNING]
> 这条是警告。

## 表格

| 名字 | 说明 | 值 |
| --- | --- | --- |
| alpha | 第一个 | `1` |
| beta | 第二个，说明长一点 | **2** |
| gamma | 第三个 | 3 |

---

最后一段，分割线上面那条是 `---`。
)";

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // **必须和 main.cpp 设的应用字体一样**，否则中文会落到宋体，
    // 截出来的图和真机不是一回事，看了等于白看。
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    // 和 main.cpp 一致，否则截出来的图不代表真机（见那边的注释）。
    appFont.setHintingPreference(QFont::PreferVerticalHinting);
    app.setFont(appFont);
#endif
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("markdown-probe.png");

    MarkdownView view;
    view.setTheme(MarkdownTheme::standard(1.0));
    view.setMaxContentWidth(760);
    view.resize(900, 400);
    view.addItem(QStringLiteral("u1"), MarkdownView::Style::Bubble,
                 QStringLiteral("帮我看下这段 Markdown 排得怎么样"));
    view.addItem(QStringLiteral("a1"), MarkdownView::Style::Document,
                 QString::fromUtf8(kSample));
    view.addItem(QStringLiteral("n1"), MarkdownView::Style::Notice,
                 QStringLiteral("这是一条提示行"));
    view.show();

    // 整篇一次画完：把视口撑到内容那么高，截出来的就是完整一页，
    // 不用一屏一屏拼。
    const int height = static_cast<int>(view.contentHeight()) + 8;
    view.resize(900, height);
    view.show();

    QImage image(view.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    view.render(&image);
    if (!image.save(output)) {
        qWarning("failed to save %s", qPrintable(output));
        return 1;
    }
    qInfo("wrote %s (%dx%d)", qPrintable(output), image.width(), image.height());
    return 0;
}
