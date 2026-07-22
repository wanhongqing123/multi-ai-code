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
