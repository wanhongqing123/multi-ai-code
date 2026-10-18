// 文件作用：验证桌面端加联系人对话框的输入门槛、紧凑布局和提交按钮状态。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QBuffer>
#include <QImage>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTest>

#include "ui/AddContactDialog.h"
#include "ui/FriendQrDecoder.h"

class AddContactDialogTest : public QObject {
    Q_OBJECT

private slots:
    void usesCustomStyledControls();
    void usesCompactDesktopGeometry();
    void enablesSubmitOnlyAfterUserIdIsEntered();
    void revealsMyAccountQrWithoutChangingInitialGeometry();
    void decodesMaiChatFriendQrFromImage();
};

void AddContactDialogTest::usesCustomStyledControls() {
    AddContactDialog dialog;
    if (qEnvironmentVariableIsSet("MAICHAT_CAPTURE_ADD_CONTACT")) {
        dialog.show();
        QCoreApplication::processEvents();
        dialog.grab().save(qEnvironmentVariable("MAICHAT_CAPTURE_ADD_CONTACT"));
    }

    QVERIFY(dialog.findChild<QLineEdit*>(QStringLiteral("contactUserIdInput")) != nullptr);
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("addContactCancelButton")) != nullptr);
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("addContactConfirmButton")) != nullptr);
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("scanFriendQrButton")) != nullptr);
    QCOMPARE(dialog.objectName(), QStringLiteral("addContactDialog"));
}

void AddContactDialogTest::usesCompactDesktopGeometry() {
    AddContactDialog dialog;

    QVERIFY(dialog.width() <= 540);
    QVERIFY(dialog.height() <= 375);
}

void AddContactDialogTest::enablesSubmitOnlyAfterUserIdIsEntered() {
    AddContactDialog dialog;

    auto* userIdInput = dialog.findChild<QLineEdit*>(QStringLiteral("contactUserIdInput"));
    auto* confirmButton = dialog.findChild<QPushButton*>(QStringLiteral("addContactConfirmButton"));
    QVERIFY(userIdInput != nullptr);
    QVERIFY(confirmButton != nullptr);
    QVERIFY(!confirmButton->isEnabled());

    QTest::keyClicks(userIdInput, QStringLiteral("  mtest_friend_12345  "));

    QVERIFY(confirmButton->isEnabled());
    QCOMPARE(dialog.userId(), QStringLiteral("mtest_friend_12345"));
}

void AddContactDialogTest::revealsMyAccountQrWithoutChangingInitialGeometry() {
    AddContactDialog dialog;
    QImage source(8, 8, QImage::Format_ARGB32);
    source.fill(Qt::white);
    QByteArray png;
    QBuffer output(&png);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&output, "PNG"));
    auto* show = dialog.findChild<QPushButton*>(QStringLiteral("showOwnFriendQrButton"));
    auto* image = dialog.findChild<QLabel*>(QStringLiteral("ownFriendQr"));
    QVERIFY(show != nullptr);
    QVERIFY(image != nullptr);
    QVERIFY(!show->isEnabled());
    dialog.setOwnQr(QStringLiteral("mtest_friend_12345"), png, {});
    QVERIFY(show->isEnabled());
    show->click();
    QVERIFY(!image->isHidden());
    QCOMPARE(dialog.height(), 625);
}

void AddContactDialogTest::decodesMaiChatFriendQrFromImage() {
    const QImage friendQr(QStringLiteral(MAICHAT_FRIEND_QR_FIXTURE));
    QVERIFY(!friendQr.isNull());
    QCOMPARE(maiChatAccountIdFromQrImage(friendQr), QStringLiteral("mtest_friend_12345"));
    QImage blank(300, 300, QImage::Format_RGB32);
    blank.fill(Qt::white);
    QVERIFY(maiChatAccountIdFromQrImage(blank).isEmpty());
}

QTEST_MAIN(AddContactDialogTest)
#include "AddContactDialogTest.moc"
