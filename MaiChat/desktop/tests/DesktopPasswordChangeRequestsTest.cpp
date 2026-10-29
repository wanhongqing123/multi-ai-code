// 文件作用：验证真实表单显示/取消、在途状态和原登录身份边界；不访问真实账号。
#include <QtTest>
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <QWindow>
#include <utility>

#include "app/DesktopPasswordChangeRequests.h"
#include "ui/PasswordChangeDialog.h"

#ifdef Q_OS_MACOS
void activateDesktopPasswordTestApplication();
QString desktopPasswordTestNativeState();
#endif

namespace {
void activateTestWindow(QWidget& window) {
    window.show();
#ifdef Q_OS_MACOS
    // 独立 QtTest 二进制没有 App bundle 的常规前台激活流程；原生激活应用，
    // 仍需下面的 ApplicationActive 断言，不伪造或放宽产品 presenter 门禁。
    activateDesktopPasswordTestApplication();
#endif
    window.raise();
    window.activateWindow();
    if (window.windowHandle()) window.windowHandle()->requestActivate();
}
}

class DesktopPasswordChangeRequestsTest : public QObject {
    Q_OBJECT
private slots:
    void actualShowAndCancelDoNotClaimAChange();
    void submittingCannotBeCanceledAndLateResultChecksLoginIdentity();
};

void DesktopPasswordChangeRequestsTest::actualShowAndCancelDoNotClaimAChange() {
    QWidget window;
    DesktopPasswordChangeRequests requests(&window);
    requests.setHandlers([] { return QStringLiteral("fixture-account"); },
        [] { return QStringLiteral("fixture-account:1"); },
        [](const QString&, DesktopPasswordChangeRequests::Completion) { QFAIL("canceled form must not submit"); });
    const QJsonObject unavailable = requests.open();
    QVERIFY(!unavailable.value(QStringLiteral("opened")).toBool());
    activateTestWindow(window);
    QTRY_VERIFY(window.isActiveWindow());
#ifdef Q_OS_MACOS
    qInfo().noquote() << desktopPasswordTestNativeState();
#endif
    QTRY_COMPARE_WITH_TIMEOUT(qApp->applicationState(), Qt::ApplicationActive, 3000);
    qInfo() << "password-form presenter" << QGuiApplication::platformName()
            << qApp->applicationState() << window.isVisible()
            << bool(QApplication::activeModalWidget());
    const QJsonObject opening = requests.open();
    QCOMPARE(opening.value(QStringLiteral("status")).toString(), QStringLiteral("opening"));
    QVERIFY(!opening.value(QStringLiteral("opened")).toBool());
    const QString id = opening.value(QStringLiteral("request_id")).toString();
    QTRY_COMPARE(requests.status(id).value(QStringLiteral("status")).toString(), QStringLiteral("awaiting_user"));
    auto* dialog = window.findChild<PasswordChangeDialog*>();
    QVERIFY(dialog && dialog->isVisible());
    const QString capture = qEnvironmentVariable("MAICHAT_PASSWORD_FORM_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(dialog->grab().save(capture));
    QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("passwordChangeNew"))->echoMode(), QLineEdit::Password);
    QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("passwordChangeConfirm"))->echoMode(), QLineEdit::Password);
    dialog->reject();
    QCOMPARE(requests.status(id).value(QStringLiteral("status")).toString(), QStringLiteral("canceled"));
    QVERIFY(!requests.status(id).value(QStringLiteral("changed")).toBool());
}

void DesktopPasswordChangeRequestsTest::submittingCannotBeCanceledAndLateResultChecksLoginIdentity() {
    QWidget window;
    activateTestWindow(window);
    QTRY_VERIFY(window.isActiveWindow());
    QTRY_COMPARE_WITH_TIMEOUT(qApp->applicationState(), Qt::ApplicationActive, 3000);
    qInfo() << "password-form presenter" << QGuiApplication::platformName()
            << qApp->applicationState() << window.isVisible()
            << bool(QApplication::activeModalWidget());
    QString identity = QStringLiteral("fixture-account:1");
    DesktopPasswordChangeRequests::Completion complete;
    int submitted = 0;
    DesktopPasswordChangeRequests requests(&window);
    requests.setHandlers([] { return QStringLiteral("fixture-account"); }, [&] { return identity; },
        [&](const QString& password, DesktopPasswordChangeRequests::Completion reply) {
            QVERIFY(password.size() >= 15);
            ++submitted; complete = std::move(reply);
        });
    const QString id = requests.open().value(QStringLiteral("request_id")).toString();
    QTRY_COMPARE(requests.status(id).value(QStringLiteral("status")).toString(), QStringLiteral("awaiting_user"));
    auto* dialog = window.findChild<PasswordChangeDialog*>();
    QVERIFY(dialog);
    auto* password = dialog->findChild<QLineEdit*>(QStringLiteral("passwordChangeNew"));
    auto* confirmation = dialog->findChild<QLineEdit*>(QStringLiteral("passwordChangeConfirm"));
    password->setText(QStringLiteral("local-safe-fixture-password"));
    confirmation->setText(password->text());
    dialog->findChild<QPushButton*>(QStringLiteral("passwordChangeSave"))->click();
    QCOMPARE(submitted, 1);
    QCOMPARE(requests.status(id).value(QStringLiteral("status")).toString(), QStringLiteral("submitting"));
    QVERIFY(password->text().isEmpty() && confirmation->text().isEmpty());
    QCOMPARE(requests.open().value(QStringLiteral("request_id")).toString(), id);
    dialog->reject();
    QVERIFY(dialog->isVisible());
    QCOMPARE(requests.status(id).value(QStringLiteral("status")).toString(), QStringLiteral("submitting"));
    identity = QStringLiteral("fixture-account:2"); // 同账号重新登录也必须隔离。
    complete({});
    QCOMPARE(requests.status(id).value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    QCOMPARE(requests.status(id).value(QStringLiteral("error_code")).toString(), QStringLiteral("identity_changed"));
    QVERIFY(!requests.status(id).value(QStringLiteral("changed")).toBool());
}

QTEST_MAIN(DesktopPasswordChangeRequestsTest)
#include "DesktopPasswordChangeRequestsTest.moc"
