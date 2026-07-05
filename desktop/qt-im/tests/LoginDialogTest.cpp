#include <QLineEdit>
#include <QPushButton>
#include <QTest>

#include "ui/LoginDialog.h"

class LoginDialogTest : public QObject {
    Q_OBJECT

private slots:
    void usesDesktopLandscapeDefaultSize();
    void enablesLoginOnlyAfterUserIdIsEntered();
};

void LoginDialogTest::usesDesktopLandscapeDefaultSize() {
    LoginDialog dialog;

    QVERIFY(dialog.width() > dialog.height());
    QVERIFY(dialog.minimumWidth() > dialog.minimumHeight());
}

void LoginDialogTest::enablesLoginOnlyAfterUserIdIsEntered() {
    LoginDialog dialog;

    auto* userIdInput = dialog.findChild<QLineEdit*>(QStringLiteral("userIdInput"));
    auto* loginButton = dialog.findChild<QPushButton*>(QStringLiteral("loginButton"));
    QVERIFY(userIdInput != nullptr);
    QVERIFY(loginButton != nullptr);
    QVERIFY(!loginButton->isEnabled());

    QTest::keyClicks(userIdInput, QStringLiteral("desktop-user"));

    QVERIFY(loginButton->isEnabled());
    QCOMPARE(dialog.userId(), QStringLiteral("desktop-user"));
}

QTEST_MAIN(LoginDialogTest)
#include "LoginDialogTest.moc"
