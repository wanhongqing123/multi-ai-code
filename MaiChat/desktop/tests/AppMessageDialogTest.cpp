#include <QtTest/QtTest>

#include <QLabel>
#include <QPushButton>

#include "ui/AppMessageDialog.h"

class AppMessageDialogTest : public QObject {
    Q_OBJECT

private slots:
    void infoShowsSingleAcknowledgeButton();
    void warningUsesWarningTitleStyle();
    void confirmExposesBothButtons();
    void destructiveConfirmDefaultsToCancel();
    void bodyTextIsSelectable();
};

void AppMessageDialogTest::infoShowsSingleAcknowledgeButton() {
    AppMessageDialog::Options options;
    options.kind = AppMessageDialog::Kind::Info;
    options.title = QStringLiteral("保存成功");
    options.body = QStringLiteral("已保存到：D:/a.png");
    AppMessageDialog dialog(options);

    // 标题必须画在面板内：系统标题栏被去掉了，漏了就没有标题可看。
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("appMessageTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), QStringLiteral("保存成功"));

    // 提示类只需一个关闭按钮，不该出现"取消"让人以为能撤销什么。
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("appMessageCancel")) == nullptr);
    auto* close = dialog.findChild<QPushButton*>(QStringLiteral("appMessageConfirm"));
    QVERIFY(close != nullptr);
    QCOMPARE(close->text(), QStringLiteral("知道了"));
}

void AppMessageDialogTest::warningUsesWarningTitleStyle() {
    AppMessageDialog::Options options;
    options.kind = AppMessageDialog::Kind::Warning;
    options.title = QStringLiteral("保存失败");
    options.body = QStringLiteral("磁盘已满");
    AppMessageDialog dialog(options);

    // 失败提示要一眼看出是失败，标题走警示色的那个 objectName。
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("appMessageTitleWarning")) != nullptr);
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("appMessageTitle")) == nullptr);
}

void AppMessageDialogTest::confirmExposesBothButtons() {
    AppMessageDialog::Options options;
    options.kind = AppMessageDialog::Kind::Confirm;
    options.title = QStringLiteral("删除消息");
    options.body = QStringLiteral("此操作不可恢复。");
    options.confirmText = QStringLiteral("删除");
    AppMessageDialog dialog(options);

    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("appMessageCancel"));
    QVERIFY(cancel != nullptr);
    QCOMPARE(cancel->text(), QStringLiteral("取消"));

    auto* confirm = dialog.findChild<QPushButton*>(QStringLiteral("appMessageConfirm"));
    QVERIFY(confirm != nullptr);
    QCOMPARE(confirm->text(), QStringLiteral("删除"));

    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    confirm->click();
    QCOMPARE(acceptedSpy.count(), 1);
}

void AppMessageDialogTest::destructiveConfirmDefaultsToCancel() {
    AppMessageDialog::Options options;
    options.kind = AppMessageDialog::Kind::Confirm;
    options.title = QStringLiteral("删除好友");
    options.body = QStringLiteral("此操作不可恢复。");
    options.confirmText = QStringLiteral("删除");
    options.destructive = true;
    AppMessageDialog dialog(options);

    // 危险动作用红色主按钮，且默认焦点必须落在取消上——
    // 误按回车不该直接把好友和聊天记录删掉。
    auto* danger = dialog.findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
    QVERIFY(danger != nullptr);
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("appMessageConfirm")) == nullptr);

    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("appMessageCancel"));
    QVERIFY(cancel != nullptr);
    QVERIFY2(cancel->isDefault(), "删除类确认的默认按钮不是取消，误按回车会直接删掉");
    QVERIFY(!danger->isDefault());
}

void AppMessageDialogTest::bodyTextIsSelectable() {
    AppMessageDialog::Options options;
    options.title = QStringLiteral("保存成功");
    options.body = QStringLiteral("已保存到：D:/very/long/path.png");
    AppMessageDialog dialog(options);

    // 路径和错误详情要能选中复制出来，否则用户只能照着屏幕手抄。
    auto* body = dialog.findChild<QLabel*>(QStringLiteral("appMessageBody"));
    QVERIFY(body != nullptr);
    QVERIFY(body->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
}

QTEST_MAIN(AppMessageDialogTest)
#include "AppMessageDialogTest.moc"
