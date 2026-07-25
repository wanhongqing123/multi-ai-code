#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QTest>
#include <QWidget>

#include "ui/LoginDialog.h"

class LoginDialogTest : public QObject {
    Q_OBJECT

private slots:
    void usesDesktopLandscapeDefaultSize();
    void matchesMultiAICodeLoginHierarchy();
    void enablesLoginOnlyAfterUserIdIsEntered();
};

void LoginDialogTest::usesDesktopLandscapeDefaultSize() {
    LoginDialog dialog;

    QVERIFY(dialog.width() > dialog.height());
    QVERIFY(dialog.minimumWidth() > dialog.minimumHeight());
}

void LoginDialogTest::matchesMultiAICodeLoginHierarchy() {
    LoginDialog dialog;

    auto* loginPanel = dialog.findChild<QWidget*>(QStringLiteral("loginPanel"));
    auto* logo = dialog.findChild<QLabel*>(QStringLiteral("loginLogo"));
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("welcomeTitle"));
    auto* userIdInput = dialog.findChild<QLineEdit*>(QStringLiteral("userIdInput"));
    auto* loginButton = dialog.findChild<QPushButton*>(QStringLiteral("loginButton"));

    QVERIFY(loginPanel != nullptr);
    QVERIFY(logo != nullptr);
    QVERIFY(title != nullptr);
    QVERIFY(userIdInput != nullptr);
    QVERIFY(loginButton != nullptr);
    QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("introPane")) == nullptr);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("MaiChat"));
    const QPixmap logoPixmap = logo->pixmap(Qt::ReturnByValue);
    QVERIFY(!logoPixmap.isNull());
    QCOMPARE(title->text(), QStringLiteral("欢迎使用 MaiChat"));
    QCOMPARE(userIdInput->placeholderText(), QStringLiteral("请输入登录账号"));
    QCOMPARE(loginButton->text(), QStringLiteral("登录"));
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
