#include <QApplication>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>
#include <QWidget>

#include "markdown/MarkdownView.h"

// 文字渲染的定点对照。
//
// 用来回答一个具体问题：**同一串 Latin，走不同的路渲染出来一样吗。**
//
// 四行内容完全一样，只有渲染方式不同：
//   1  QLabel                     Qt 自己那条路，基准
//   2  MarkdownView 纯文本        一个格式段
//   3  MarkdownView 前面带样式    同一段里有多个格式段（粗体、行内代码…）
//   4  MarkdownView 行内代码紧邻  格式段边界就压在 Latin 单词旁边
//
// 字间距如果只在 3、4 里变形，说明问题出在「一段里分了多个格式段」——
// Qt 会把每个格式段当成独立的 item 去 shape，跨边界的字距就对不上了。

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    app.setFont(appFont);
#endif
    const QString output =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("text-probe.png");

    const QString plain = QStringLiteral("这是 QTextDocument 的 CSS 子集画不出来的东西之一。");
    const MarkdownTheme theme = MarkdownTheme::standard(1.0);

    auto* host = new QWidget;
    host->setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* column = new QVBoxLayout(host);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    const auto addTag = [&column, host](const QString& text) {
        auto* tag = new QLabel(text, host);
        tag->setStyleSheet(QStringLiteral("color:#98a2b3;padding:2px 8px;background:#ffffff;"));
        QFont small = tag->font();
        small.setPixelSize(10);
        tag->setFont(small);
        column->addWidget(tag);
    };

    const auto addView = [&column, host, &theme](const QString& markdown) {
        auto* view = new MarkdownView(host);
        view->setTheme(theme);
        view->addItem(QStringLiteral("a"), MarkdownView::Style::Document, markdown);
        view->setFixedHeight(34);
        column->addWidget(view);
    };

    addTag(QStringLiteral("1  QLabel"));
    auto* label = new QLabel(plain, host);
    QFont bodyFont;
    bodyFont.setPixelSize(theme.bodyPixelSize);
    label->setFont(bodyFont);
    label->setStyleSheet(QStringLiteral("color:#0e1525;background:#ffffff;padding:0 16px;"));
    column->addWidget(label);

    addTag(QStringLiteral("2  MarkdownView / 纯文本"));
    addView(plain);

    addTag(QStringLiteral("3  MarkdownView / 前面有别的样式"));
    addView(QStringLiteral("**粗** ") + plain);

    addTag(QStringLiteral("4  MarkdownView / 行内代码紧挨着"));
    addView(QStringLiteral("这是 `QTextDocument` 的 CSS 子集画不出来的东西之一。"));

    // 5 / 6：换字形提示策略。Qt 在 Windows 上默认走全提示，
    // 每个字的步进被四舍五入到整像素，字距就忽宽忽窄。
    addTag(QStringLiteral("5  QLabel / PreferNoHinting"));
    auto* noHint = new QLabel(plain, host);
    QFont noHintFont = bodyFont;
    noHintFont.setHintingPreference(QFont::PreferNoHinting);
    noHint->setFont(noHintFont);
    noHint->setStyleSheet(QStringLiteral("color:#0e1525;background:#ffffff;padding:0 16px;"));
    column->addWidget(noHint);

    addTag(QStringLiteral("6  QLabel / PreferVerticalHinting"));
    auto* vHint = new QLabel(plain, host);
    QFont vHintFont = bodyFont;
    vHintFont.setHintingPreference(QFont::PreferVerticalHinting);
    vHint->setFont(vHintFont);
    vHint->setStyleSheet(QStringLiteral("color:#0e1525;background:#ffffff;padding:0 16px;"));
    column->addWidget(vHint);

    host->resize(560, 360);
    host->show();

    QImage image(host->size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    host->render(&image);
    if (!image.save(output)) {
        qWarning("failed to save %s", qPrintable(output));
        return 1;
    }
    qInfo("wrote %s", qPrintable(output));
    return 0;
}
