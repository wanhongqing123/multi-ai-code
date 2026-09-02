#include <QtTest/QtTest>

#include <QApplication>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSizeGrip>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextTable>
#include <QtMath>

#include "ui/FilePreviewDialog.h"

class FilePreviewDialogTest : public QObject {
    Q_OBJECT

private slots:
    void dropsNativeTitleBarAndShowsNameOnce();
    void rendersHtmlContent();
    void rendersSideBySideDiffTable();
    void normalizesGitDiffHtmlForQt();
    void keepsEveryFileWhenTheDiffHasManyFiles();
    void longFileNameIsElidedInsteadOfWideningWindow();
    void initialSizeTracksContentAndKeepsDiffReadable();
    void closeButtonsAccept();
    void escapeStillCloses();
    void stillResizableWithoutSystemFrame();
    void draggingAnyEdgeResizesTheWindow();
    void resizeIsClampedToTheMinimumSize();
    void pressInTheMiddleDoesNotResize();
    void multiFileDiffPutsTheFileListOnTheLeft();
    void clickingAFileInTheListJumpsToItsDiff();
    void fileTreeCollapsesSingleChildDirectories();
    void plainDocumentHasNoFileList();
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

// 多文件 Diff 必须每个文件都留下来。
//
// 剥离 unified 块的正则原来是贪婪的 `.*`，而每个文件各有一对 START/END 标记：
// 它会从第一个 START 一路吃到最后一个 END，把中间所有文件连同它们的 split 表格
// 一起删掉。21 个文件的真实报告实测被吞掉 97.2%，桌面端只剩第一个文件可看，
// 而且不报错、不空白——看起来就像「这次只改了一个文件」。
//
// 原来的用例只有一个文件，贪婪与非贪婪表现完全一致，所以这个 bug 一直没被发现。
// 这条用例的关键就是**至少三个文件**。
void FilePreviewDialogTest::keepsEveryFileWhenTheDiffHasManyFiles() {
    QString source;
    for (int i = 0; i < 3; ++i) {
        source += QStringLiteral(
                      "<div class='file'><a name='f%1'></a>"
                      "<!-- MAICHAT_SPLIT_START --><div>split-%1</div><!-- MAICHAT_SPLIT_END -->"
                      "<!-- MAICHAT_UNIFIED_START --><div>mobile-%1</div><!-- MAICHAT_UNIFIED_END -->"
                      "</div>")
                      .arg(i);
    }

    const QString normalized = FilePreviewDialog::normalizeGitDiffHtmlForQt(source);

    for (int i = 0; i < 3; ++i) {
        QVERIFY2(normalized.contains(QStringLiteral("split-%1").arg(i)),
                 qPrintable(QStringLiteral("第 %1 个文件的对比表格被剥离步骤吃掉了").arg(i)));
        QVERIFY2(normalized.contains(QStringLiteral("<a name='f%1'></a>").arg(i)),
                 qPrintable(QStringLiteral("第 %1 个文件的锚点丢了，索引跳不过去").arg(i)));
        QVERIFY(!normalized.contains(QStringLiteral("mobile-%1").arg(i)));
    }
    QVERIFY(!normalized.contains(QStringLiteral("MAICHAT_UNIFIED_START")));
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

void FilePreviewDialogTest::initialSizeTracksContentAndKeepsDiffReadable() {
    QWidget parent;
    parent.resize(1200, 820);

    FilePreviewDialog shortDocument(
        QStringLiteral("short.md"),
        QStringLiteral("<h2>简短说明</h2><p>只有一行正文。</p>"),
        &parent);

    QString longHtml = QStringLiteral("<h2>长文档</h2>");
    for (int i = 0; i < 120; ++i) {
        longHtml += QStringLiteral("<p>第 %1 行：用于验证长内容会扩展到屏幕安全高度并滚动。</p>")
                        .arg(i + 1);
    }
    FilePreviewDialog longDocument(QStringLiteral("long.md"), longHtml, &parent);

    const QString diffHtml = QStringLiteral(
        "<table><tr><td>1</td><td>before</td><td>1</td><td>after</td></tr></table>");
    FilePreviewDialog diffDocument(
        QStringLiteral("remote-im-diff-repo.html"), diffHtml, &parent);

    QVERIFY2(shortDocument.height() < longDocument.height(),
             "短文档与长文档仍使用同一个固定初始高度");
    QVERIFY2(diffDocument.width() > shortDocument.width(),
             "Diff 没有获得左右对比所需的可读初始宽度");

    longDocument.show();
    QVERIFY(QTest::qWaitForWindowExposed(&longDocument));
    auto* longContent = longDocument.findChild<QTextBrowser*>(
        QStringLiteral("filePreviewContent"));
    QVERIFY(longContent != nullptr);
    QVERIFY2(longContent->verticalScrollBar()->maximum() > 0,
             "长文档达到屏幕安全高度后没有提供垂直滚动");

    const QSize manualSize = shortDocument.size() + QSize(40, 30);
    shortDocument.resize(manualSize);
    QCoreApplication::processEvents();
    QCOMPARE(shortDocument.size(), manualSize);

    QScreen* screen = QApplication::screenAt(
        parent.mapToGlobal(parent.rect().center()));
    if (!screen) screen = QApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QSize available = screen->availableGeometry().size();
    QVERIFY2(longDocument.width() <= qMax(1, qFloor(available.width() * 0.90)) + 1,
             "预览窗口初始宽度越过屏幕安全范围");
    QVERIFY2(longDocument.height() <= qMax(1, qFloor(available.height() * 0.90)) + 1,
             "预览窗口初始高度越过屏幕安全范围");
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

namespace {

// 用显式的全局坐标发事件：拖上边/左边时窗口原点会跟着动，
// 若在每一步用 mapToGlobal 现算，第二次算出来的全局点就已经偏了。
void sendMouseAt(QWidget* widget, QEvent::Type type, const QPoint& global,
                 Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, widget->mapFromGlobal(global), global, button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void dragEdge(QWidget* widget, const QPoint& grabLocal, const QPoint& delta) {
    const QPoint start = widget->mapToGlobal(grabLocal);
    sendMouseAt(widget, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouseAt(widget, QEvent::MouseMove, start + delta, Qt::NoButton, Qt::LeftButton);
    sendMouseAt(widget, QEvent::MouseButtonRelease, start + delta, Qt::LeftButton, Qt::NoButton);
}

}  // namespace

namespace {

// 按生成器真实写出的结构造报告：索引行的标记必须和 gitDiffReport.ts 一致，
// 自己另编一套等于把解析器对着自己的想象测。
QString diffReportHtmlFor(const QStringList& labels, int bodyLines) {
    const int fileCount = labels.size();
    QString index = QStringLiteral(
        "<div class=\"file-index\"><div class=\"file-index-title\">变更文件（%1）</div><ul>")
        .arg(fileCount);
    QString body;
    for (int i = 0; i < fileCount; ++i) {
        index += QStringLiteral(
                     "<li><a href=\"#f%1\">%4</a>"
                     "<span class=\"idx-stat\"> &nbsp;&nbsp;"
                     "<span class=\"idx-add\">+%2</span> "
                     "<span class=\"idx-del\">-%3</span></span></li>")
                     .arg(i)
                     .arg(10 + i)
                     .arg(i)
                     .arg(labels.at(i));
        body += QStringLiteral("<div class=\"file\" id=\"f%1\"><a name=\"f%1\"></a>"
                               "<div class=\"file-title\">%2</div>")
                    .arg(i)
                    .arg(labels.at(i));
        for (int line = 0; line < bodyLines; ++line) {
            body += QStringLiteral("<p>file %1 line %2</p>").arg(i).arg(line);
        }
        body += QStringLiteral("</div>");
    }
    index += QStringLiteral("</ul></div>");
    return index + body;
}

QString diffReportHtml(int fileCount, int bodyLines) {
    QStringList labels;
    for (int i = 0; i < fileCount; ++i) {
        labels << QStringLiteral("pkg/module%1/source%1.ts").arg(i);
    }
    return diffReportHtmlFor(labels, bodyLines);
}

// 树里叶子的位置随目录折叠而变，按锚点找才不会写死结构。
QTreeWidgetItem* leafForAnchor(QTreeWidget* tree, const QString& anchor) {
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole).toString() == anchor) return *it;
        ++it;
    }
    return nullptr;
}

}  // namespace

void FilePreviewDialogTest::multiFileDiffPutsTheFileListOnTheLeft() {
    FilePreviewDialog dialog(QStringLiteral("remote-im-diff-demo.html"), diffReportHtml(3, 4));

    auto* splitter = dialog.findChild<QSplitter*>(QStringLiteral("filePreviewSplitter"));
    QVERIFY2(splitter != nullptr, "多文件 Diff 应当是左右两栏");
    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("filePreviewFileList"));
    QVERIFY2(list != nullptr, "左栏没有文件列表");
    auto* content = dialog.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(content != nullptr);

    // 顺序不能反：目录在左、正文在右。
    QCOMPARE(splitter->indexOf(list), 0);
    QCOMPARE(splitter->indexOf(content), 1);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    // 任一栏被拖没就没有恢复入口了。
    QVERIFY(!splitter->childrenCollapsible());

    // 目录成树，不是把整条路径平铺：根下只有 pkg/ 一个节点。
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("pkg/"));
    QCOMPARE(list->topLevelItem(0)->childCount(), 3);

    QTreeWidgetItem* first = leafForAnchor(list, QStringLiteral("f0"));
    QVERIFY(first != nullptr);
    // 叶子只显示文件名；完整路径退到 tooltip，不占那一行的宽度。
    QCOMPARE(first->text(0), QStringLiteral("source0.ts"));
    QCOMPARE(first->toolTip(0), QStringLiteral("pkg/module0/source0.ts"));
    // 增删行数要跟着，否则这个列表只是个文件名清单。
    QCOMPARE(leafForAnchor(list, QStringLiteral("f2"))->text(1), QStringLiteral("+12 -2"));

    // 左栏顶替了文档顶部那份索引：两份并排会让人怀疑哪份是准的。
    QVERIFY2(!content->toPlainText().contains(QStringLiteral("变更文件（3）")),
             "文档里那份索引没被剥掉，和左栏重复了");
    // 但正文本身一个文件都不能少。
    for (int i = 0; i < 3; ++i) {
        QVERIFY2(content->toPlainText().contains(QStringLiteral("pkg/module%1/source%1.ts").arg(i)),
                 qPrintable(QStringLiteral("剥索引把第 %1 个文件也带走了").arg(i)));
    }
}

void FilePreviewDialogTest::clickingAFileInTheListJumpsToItsDiff() {
    // 正文要足够长，短文档根本滚不动，断言就恒真了。
    FilePreviewDialog dialog(QStringLiteral("remote-im-diff-demo.html"), diffReportHtml(4, 120));
    dialog.resize(900, 500);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("filePreviewFileList"));
    auto* content = dialog.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(list != nullptr && content != nullptr);
    QVERIFY2(content->verticalScrollBar()->maximum() > 0, "正文没到能滚动的长度，用例证明不了跳转");
    QCOMPARE(content->verticalScrollBar()->value(), 0);

    list->setCurrentItem(leafForAnchor(list, QStringLiteral("f3")));
    const int atLast = content->verticalScrollBar()->value();
    QVERIFY2(atLast > 0, "点了左栏最后一个文件，右侧正文没动");

    // 再点回第一个必须往回走，不然只能证明「往下滚过一次」，证明不了是在按文件定位。
    list->setCurrentItem(leafForAnchor(list, QStringLiteral("f0")));
    QVERIFY2(content->verticalScrollBar()->value() < atLast,
             "点回第一个文件没有往回滚，说明跳转不是按文件定位的");
}

void FilePreviewDialogTest::fileTreeCollapsesSingleChildDirectories() {
    FilePreviewDialog dialog(
        QStringLiteral("remote-im-diff-demo.html"),
        diffReportHtmlFor({QStringLiteral("electron/remote-im/router.ts"),
                           QStringLiteral("electron/remote-im/replyProtocol.ts"),
                           QStringLiteral("package.json")},
                          4));

    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("filePreviewFileList"));
    QVERIFY(list != nullptr);

    // electron/ 下只有 remote-im/ 一个孩子，两层各占一行纯粹是缩进噪声，
    // 必须并成 electron/remote-im/ 一行。
    QCOMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("electron/remote-im/"));
    QCOMPARE(list->topLevelItem(0)->childCount(), 2);
    // 仓库根下的文件不该被硬塞进某个目录节点。
    QCOMPARE(list->topLevelItem(1)->text(0), QStringLiteral("package.json"));
    QCOMPARE(list->topLevelItem(1)->data(0, Qt::UserRole).toString(), QStringLiteral("f2"));

    QCOMPARE(leafForAnchor(list, QStringLiteral("f0"))->text(0), QStringLiteral("router.ts"));
    QVERIFY2(list->topLevelItem(0)->isExpanded(), "树默认要展开，否则还得一层层点开");
}

void FilePreviewDialogTest::plainDocumentHasNoFileList() {
    // 普通 Markdown/HTML 附件没有文件索引，左栏纯占地方。
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));
    QVERIFY(dialog.findChild<QTreeWidget*>(QStringLiteral("filePreviewFileList")) == nullptr);
    QVERIFY(dialog.findChild<QSplitter*>(QStringLiteral("filePreviewSplitter")) == nullptr);

    // 只有一个文件时也不给左栏：列表里只有一行，点它等于原地不动。
    FilePreviewDialog single(QStringLiteral("remote-im-diff-demo.html"), diffReportHtml(1, 4));
    QVERIFY(single.findChild<QTreeWidget*>(QStringLiteral("filePreviewFileList")) == nullptr);
}

void FilePreviewDialogTest::draggingAnyEdgeResizesTheWindow() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    // 右边缘。外圈是对话框自己的透明区，子部件不该盖住它，
    // 否则真实鼠标根本点不到——这一条是「看得见的handle」测不出来的。
    const QPoint rightEdge(dialog.width() - 2, dialog.height() / 2);
    QVERIFY2(dialog.childAt(rightEdge) == nullptr, "右边缘被子部件盖住，真实鼠标点不到");

    QSize before = dialog.size();
    dragEdge(&dialog, rightEdge, QPoint(120, 0));
    QCOMPARE(dialog.width(), before.width() + 120);
    QCOMPARE(dialog.height(), before.height());

    // 下边缘。
    before = dialog.size();
    dragEdge(&dialog, QPoint(dialog.width() / 2, dialog.height() - 2), QPoint(0, 80));
    QCOMPARE(dialog.height(), before.height() + 80);
    QCOMPARE(dialog.width(), before.width());

    // 左边缘：变宽的同时窗口原点要跟着左移，否则就是「右边被拉长了」。
    before = dialog.size();
    const int leftBefore = dialog.geometry().left();
    dragEdge(&dialog, QPoint(2, dialog.height() / 2), QPoint(-100, 0));
    QCOMPARE(dialog.width(), before.width() + 100);
    QCOMPARE(dialog.geometry().left(), leftBefore - 100);
}

void FilePreviewDialogTest::resizeIsClampedToTheMinimumSize() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    // 拖右边／下边过头只需要尺寸不塌——这一半其实是 Qt 的 setGeometry
    // 自己按 minimumSize 夹住的，断言它并不能证明我们的夹取逻辑存在。
    dragEdge(&dialog, QPoint(dialog.width() - 2, dialog.height() / 2), QPoint(-100000, 0));
    QCOMPARE(dialog.width(), dialog.minimumWidth());
    dragEdge(&dialog, QPoint(dialog.width() / 2, dialog.height() - 2), QPoint(0, -100000));
    QCOMPARE(dialog.height(), dialog.minimumHeight());

    // 真正要自己夹的是**位置**：左／上边往里拖过头时，只夹尺寸的话 Qt 会保持
    // 最小尺寸、却让 topLeft 继续跟着鼠标滑走，窗口就一路飘出去了。
    QRect before = dialog.geometry();
    dragEdge(&dialog, QPoint(2, dialog.height() / 2), QPoint(100000, 0));
    QCOMPARE(dialog.width(), dialog.minimumWidth());
    QCOMPARE(dialog.geometry().left(), before.right() - dialog.minimumWidth() + 1);

    before = dialog.geometry();
    dragEdge(&dialog, QPoint(dialog.width() / 2, 2), QPoint(0, 100000));
    QCOMPARE(dialog.height(), dialog.minimumHeight());
    QCOMPARE(dialog.geometry().top(), before.bottom() - dialog.minimumHeight() + 1);
}

void FilePreviewDialogTest::pressInTheMiddleDoesNotResize() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    const QSize before = dialog.size();
    // 正文中央离每条边都很远：在这里按住拖动是选中文字，不该动窗口尺寸。
    dragEdge(&dialog, dialog.rect().center(), QPoint(150, 150));
    QCOMPARE(dialog.size(), before);
}

void FilePreviewDialogTest::stillResizableWithoutSystemFrame() {
    FilePreviewDialog dialog(QStringLiteral("report.md"), QStringLiteral("<p>hi</p>"));

    // 无边框窗口没有系统缩放边框；少了 grip，长文档就只能在固定大小里滚，
    // 比原来的原生窗口更难用。
    auto* grip = dialog.findChild<QSizeGrip*>(QStringLiteral("filePreviewGrip"));
    QVERIFY2(grip != nullptr,
             "无边框预览窗没有缩放入口");
    QCOMPARE(grip->cursor().shape(), Qt::SizeFDiagCursor);
    QVERIFY2(!grip->toolTip().isEmpty(), "缩放入口没有向用户说明可以拖动调整大小");
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
