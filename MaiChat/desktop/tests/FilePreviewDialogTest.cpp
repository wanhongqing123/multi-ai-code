#include <QtTest/QtTest>

#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSizeGrip>
#include <QTextBrowser>

#include "ui/FilePreviewDialog.h"

class FilePreviewDialogTest : public QObject {
    Q_OBJECT

private slots:
    void dropsNativeTitleBarAndShowsNameOnce();
    void rendersHtmlContent();
    void longFileNameIsElidedInsteadOfWideningWindow();
    void closeButtonsAccept();
    void stillResizableWithoutSystemFrame();
    void panelActuallyPaintsItsBackground();
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

    // 标题栏右上角那个 ✕ 是去掉系统标题栏后唯一的「角落关闭」入口，不能漏。
    auto* closeIcon = dialog.findChild<QPushButton*>(QStringLiteral("filePreviewCloseIcon"));
    QVERIFY(closeIcon != nullptr);

    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    closeIcon->click();
    QCOMPARE(acceptedSpy.count(), 1);
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

QTEST_MAIN(FilePreviewDialogTest)
#include "FilePreviewDialogTest.moc"
