// 文件作用：验证桌面端加联系人对话框的输入门槛、紧凑布局和提交按钮状态。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QLineEdit>
#include <QPushButton>
#include <QTest>

#include "ui/AddContactDialog.h"

class AddContactDialogTest : public QObject {
    Q_OBJECT

private slots:
    void usesCustomStyledControls();
    void usesCompactDesktopGeometry();
    void enablesSubmitOnlyAfterUserIdIsEntered();
};

void AddContactDialogTest::usesCustomStyledControls() {
    AddContactDialog dialog;

    QVERIFY(dialog.findChild<QLineEdit*>(QStringLiteral("contactUserIdInput")) != nullptr);
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("addContactCancelButton")) != nullptr);
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("addContactConfirmButton")) != nullptr);
    QCOMPARE(dialog.objectName(), QStringLiteral("addContactDialog"));
}

void AddContactDialogTest::usesCompactDesktopGeometry() {
    AddContactDialog dialog;

    QVERIFY(dialog.width() <= 540);
    QVERIFY(dialog.height() <= 340);
}

void AddContactDialogTest::enablesSubmitOnlyAfterUserIdIsEntered() {
    AddContactDialog dialog;

    auto* userIdInput = dialog.findChild<QLineEdit*>(QStringLiteral("contactUserIdInput"));
    auto* confirmButton = dialog.findChild<QPushButton*>(QStringLiteral("addContactConfirmButton"));
    QVERIFY(userIdInput != nullptr);
    QVERIFY(confirmButton != nullptr);
    QVERIFY(!confirmButton->isEnabled());

    QTest::keyClicks(userIdInput, QStringLiteral("  whq-iphone  "));

    QVERIFY(confirmButton->isEnabled());
    QCOMPARE(dialog.userId(), QStringLiteral("whq-iphone"));
}

QTEST_MAIN(AddContactDialogTest)
#include "AddContactDialogTest.moc"
