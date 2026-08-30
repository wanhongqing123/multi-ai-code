#include <QtTest/QtTest>

#include <QApplication>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizeGrip>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextTable>

#include "ui/FilePreviewDialog.h"

class FilePreviewDialogTest : public QObject {
    Q_OBJECT

private slots:
    void dropsNativeTitleBarAndShowsNameOnce();
    void rendersHtmlContent();
    void rendersSideBySideDiffTable();
    void normalizesGitDiffHtmlForQt();
    void longFileNameIsElidedInsteadOfWideningWindow();
    void closeButtonsAccept();
    void escapeStillCloses();
    void stillResizableWithoutSystemFrame();
    void panelActuallyPaintsItsBackground();
    void contentFontFollowsApplicationFont();
};

void FilePreviewDialogTest::dropsNativeTitleBarAndShowsNameOnce() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));

    // 系统标题栏会挂「?」帮助按钮、并把文件名重复显示两遍，必须去掉。
    QVERIFY2(dialog.windowFlags().testFlag(Qt::FramelessWindowHint),
             "预览窗仍带系统标题栏，会重新出现「?」按钮和重复标题");

    // 去掉标题栏后，文件名只能由面板内的标签承担，漏了就完全看不到是哪个文件。
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("filePreviewTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), QStringLiteral("report.md"));
}

void FilePreviewDialogTest::rendersHtmlContent() {
    FilePreviewDialog dialog(QStringLiteral("report.md"),
                             QStringLiteral("<h1>标题</h1><p>正文内容</p>"));

    auto* content = dialog.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(content != nullptr);
    QVERIFY(content->isReadOnly());
    QVERIFY(content->toPlainText().contains(QStringLiteral("正文内容")));
}

void FilePreviewDialogTest::rendersSideBySideDiffTable() {
    const QString html = QStringLiteral(
        "<section><div>src/app.cpp</div><table><tr>"
        "<td>10</td><td><pre>before</pre></td>"
        "<td>10</td><td><pre>after</pre></td>"
        "</tr></table></section>");
    FilePreviewDialog dialog(QStringLiteral("remote-im-diff-repo.html"), html);

    auto* content = dialog.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(content != nullptr);
    const QTextCursor before = content->document()->find(QStringLiteral("before"));
    const QTextCursor after = content->document()->find(QStringLiteral("after"));
    QVERIFY2(!before.isNull() && !after.isNull(), "Diff 两侧代码没有进入 Qt 文档");
    QVERIFY2(before.currentTable() != nullptr, "删除侧没有落在表格中，无法左右对比");
    QCOMPARE(before.currentTable(), after.currentTable());
    QCOMPARE(before.currentTable()->columns(), 4);
}

void FilePreviewDialogTest::normalizesGitDiffHtmlForQt() {
    const QString source = QStringLiteral(
        "<style>body{background:var(--bg);color:var(--text)}"
        ".pill{border:1px solid var(--border);color:var(--muted)}"
        ".qt-separator{display:none}.add{background:var(--add)}"
        ".palette{background:var(--panel);border-color:var(--del);color:var(--hunk);"
        "outline-color:var(--add-strong);text-decoration-color:var(--del-strong)}</style>"
        "<div class='title'><span class='pill'>提交 abc</span>"
        "<span class='qt-separator'> · </span><span class='pill'>1 files</span>"
        "<span class='qt-separator'> · </span><span class='pill'>+3 / -1</span></div>"
        "<!-- MAICHAT_SPLIT_START --><div>split-only</div><!-- MAICHAT_SPLIT_END -->"
        "<!-- MAICHAT_UNIFIED_START --><div>mobile-only</div><!-- MAICHAT_UNIFIED_END -->");

    const QString normalized = FilePreviewDialog::normalizeGitDiffHtmlForQt(source);
    QVERIFY(normalized.contains(QStringLiteral("split-only")));
    QVERIFY(!normalized.contains(QStringLiteral("mobile-only")));
    QVERIFY(!normalized.contains(QStringLiteral("MAICHAT_UNIFIED_START")));
    QVERIFY2(!normalized.contains(QStringLiteral("var(--")),
             "Qt 不支持的 CSS 自定义属性仍残留在预览 HTML 中");
    QVERIFY(normalized.contains(QStringLiteral("#d0d7de")));
    QVERIFY(normalized.contains(QStringLiteral("#656d76")));
    QVERIFY(normalized.contains(QStringLiteral("#dafbe1")));
    QVERIFY2(!normalized.contains(QStringLiteral(".qt-separator{display:none}")),
             "Qt 真实文本分隔符仍被隐藏");

    FilePreviewDialog dialog(QStringLiteral("remote-im-diff-repo.html"), normalized);
    auto* content = dialog.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(content != nullptr);
    QString rendered = content->toPlainText();
    rendered.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    QVERIFY2(rendered.contains(QStringLiteral("提交 abc · 1 files · +3 / -1")),
             qPrintable(QStringLiteral("Qt 中的 Diff 摘要仍粘连：%1").arg(rendered)));
}

void FilePreviewDialogTest::longFileNameIsElidedInsteadOfWideningWindow() {
    const QString longName =
        QStringLiteral("trtc_qos_v1_delay_based_congestion_detection_and_trendline_filter_notes.md");
    FilePreviewDialog dialog(longName, QStringLiteral("<p>hi</p>"));
    dialog.resize(560, 420);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto* title = dialog.findChild<QLabel*>(QStringLiteral("filePreviewTitle"));
    QVERIFY(title != nullptr);
    // 超长文件名必须省略，而不是把窗口撑宽——否则一条长文件名就能把预览窗顶出屏幕。
    QVERIFY2(title->width() <= dialog.width(), "标题把窗口撑宽了");
    QVERIFY2(title->text() != longName, "超长文件名没有被省略");
    // 中间省略：末尾省略会把扩展名吃掉，看不出是 md 还是 html。
    QVERIFY2(title->text().endsWith(QStringLiteral(".md")), "省略方式吃掉了扩展名");
}

void FilePreviewDialogTest::closeButtonsAccept() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));

    auto* close = dialog.findChild<QPushButton*>(QStringLiteral("filePreviewClose"));
    QVERIFY(close != nullptr);
    QCOMPARE(close->text(), QStringLiteral("关闭"));

    // 关闭出口只留底部这一个：标题栏再放一个 ✕ 就是并排的第二个出口，
    // 反而让人犹豫该点哪个（远程观看窗那次已经做过同样的取舍）。
    QVERIFY2(dialog.findChild<QPushButton*>(QStringLiteral("filePreviewCloseIcon")) == nullptr,
             "标题栏又出现了重复的关闭入口");

    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    close->click();
    QCOMPARE(acceptedSpy.count(), 1);
}

void FilePreviewDialogTest::escapeStillCloses() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    // 去掉标题栏的 ✕ 之后，键盘上只剩 Esc 这一条退路，不能连它也失效。
    QSignalSpy finishedSpy(&dialog, &QDialog::finished);
    QTest::keyClick(&dialog, Qt::Key_Escape);
    QCOMPARE(finishedSpy.count(), 1);
}

void FilePreviewDialogTest::stillResizableWithoutSystemFrame() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));

    // 无边框窗口没有系统缩放边框；少了 grip，长文档就只能在固定大小里滚，
    // 比原来的原生窗口更难用。
    QVERIFY2(dialog.findChild<QSizeGrip*>(QStringLiteral("filePreviewGrip")) != nullptr,
             "无边框预览窗没有缩放入口");
}

void FilePreviewDialogTest::panelActuallyPaintsItsBackground() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));
    dialog.resize(600, 460);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    // 只断言「控件存在」抓不到 QWidget 子类不画样式表背景这类坑（红条那次就是这么漏的）：
    // 窗口开了 WA_TranslucentBackground，圆角面板要是没画出来，整个预览就是一片透明。
    const QImage shot = dialog.grab().toImage();
    const QColor center = shot.pixelColor(shot.width() / 2, shot.height() / 2);
    QVERIFY2(center.alpha() > 0, "面板中心是全透明的，说明圆角面板根本没画出来");
}

void FilePreviewDialogTest::contentFontFollowsApplicationFont() {
    // MarkdownRenderer 的 CSS 只声明字号、不声明 font-family，正文字体族完全由
    // 文档默认字体决定。漏了这一步就会落到 Qt 在中文 Windows 上的默认宋体，
    // 衬线观感与界面其余部分（Segoe UI + 微软雅黑，见 main.cpp）脱节。
    const QFont previous = QApplication::font();
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    QApplication::setFont(appFont);

    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>正文</p>"));
    auto* content = dialog.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(content != nullptr);
    QCOMPARE(content->document()->defaultFont().families(), appFont.families());

    QApplication::setFont(previous);
}

QTEST_MAIN(FilePreviewDialogTest)
#include "FilePreviewDialogTest.moc"
