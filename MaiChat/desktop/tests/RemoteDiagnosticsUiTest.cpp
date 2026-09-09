#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include "app/RemoteIMApplication.h"
#include "diagnostics/RemoteDiagnosticsProtocol.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"
#include "ui/RemoteDiagnosticsDialog.h"

class RemoteDiagnosticsUiTest : public QObject {
    Q_OBJECT
    void throughRealMenu(bool confirm, bool switchAccount = false, bool toPeer = false, bool onlyPeer = false) {
        auto client = std::make_unique<FakeRemoteIMClient>(); auto* fake = client.get();
        RemoteIMApplication app("phone", std::move(client));
        app.addContact("machine", "Faulty computer");
        if (!onlyPeer) { app.addContact("helper", "Same display name"); app.addContact("other", "Same display name"); }
        const QString recipient = toPeer ? "machine" : "helper";
        app.connectToService(123, "test-only-signature"); app.selectPeer("machine");
        MainWindow window(app); window.resize(1100, 800); window.show();
        QTest::qWait(50);
        auto* more = window.findChild<QPushButton*>("moreButton"); QVERIFY(more); QVERIFY(more->isVisible());
        bool menuSeen = false, dialogSeen = false, timedOut = false;
        QTimer menuDriver, dialogDriver, watchdog;
        menuDriver.setInterval(10); dialogDriver.setInterval(10); watchdog.setSingleShot(true);
        connect(&menuDriver, &QTimer::timeout, &window, [&] {
            if (menuSeen) return;
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* menu = qobject_cast<QMenu*>(widget);
                if (!menu || !menu->isVisible()) continue;
                for (auto* action : menu->actions()) {
                    if (action->text() != QStringLiteral("远程排障")) continue;
                    menuSeen = true; QVERIFY(action->isEnabled());
                    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
                    return;
                }
            }
        });
        connect(&dialogDriver, &QTimer::timeout, &window, [&] {
            if (dialogSeen) return;
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<RemoteDiagnosticsDialog*>(widget);
                if (!dialog || !dialog->isVisible()) continue;
                dialogSeen = true;
                auto* recipients = dialog->findChild<QListWidget*>("diagnosticsRecipients");
                auto* submit = dialog->findChild<QPushButton*>("diagnosticsConfirm");
                auto* cancel = dialog->findChild<QPushButton*>("diagnosticsCancel");
                QVERIFY(recipients); QVERIFY(submit); QVERIFY(cancel);
                QVERIFY(!submit->isEnabled()); QCOMPARE(recipients->count(), onlyPeer ? 1 : 3);
                int selected = -1;
                for (int i = 0; i < recipients->count(); ++i) {

                    if (recipients->item(i)->text().contains("(" + recipient + ")")) selected = i;
                }
                QVERIFY(selected >= 0);
                QTest::mouseClick(recipients->viewport(), Qt::LeftButton, Qt::NoModifier,
                    recipients->visualItemRect(recipients->item(selected)).center());
                QVERIFY(submit->isEnabled()); QCOMPARE(dialog->selectedRecipientId(), recipient);
                QVERIFY(dialog->findChild<QLabel*>("diagnosticsSummary")->text().contains(recipient));
                const auto screenshot = qEnvironmentVariable("MAICHAT_DIAGNOSTICS_UI_SCREENSHOT");
                if (confirm && !screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot + "-confirm.png"));
                // Even an external chat switch while confirming cannot change B.
                if (!onlyPeer) app.selectPeer("other");
                if (switchAccount) app.connectToService(124, "another-test-signature");
                QTest::mouseClick(confirm ? submit : cancel, Qt::LeftButton);
                return;
            }
        });
        connect(&watchdog, &QTimer::timeout, &window, [&] {
            timedOut = true;
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
                if (auto* menu = qobject_cast<QMenu*>(widget)) menu->close();
            }
        });
        menuDriver.start(); dialogDriver.start(); watchdog.start(3000);
        QTest::mouseClick(more, Qt::LeftButton);
        menuDriver.stop(); dialogDriver.stop(); watchdog.stop();
        QVERIFY(!timedOut); QVERIFY(menuSeen); QVERIFY(dialogSeen);
        auto* controller = window.findChild<RemoteDiagnosticsController*>();
        if (!confirm || switchAccount) {
            QVERIFY(!controller); QVERIFY(fake->lastText().isEmpty()); QVERIFY(fake->lastFilePath().isEmpty());
            if (switchAccount) QVERIFY(window.findChild<QLabel*>("remoteDiagnosticsStatus")->text().contains(QStringLiteral("账号已变更")));
            return;
        }
        QVERIFY(controller); QTRY_VERIFY(fake->lastText().startsWith("/diagnostics "));
        QCOMPARE(fake->lastTextPeerId(), QString("machine"));
        auto* status = window.findChild<QLabel*>("remoteDiagnosticsStatus"); QVERIFY(status);
        QVERIFY(status->isVisible()); QVERIFY(status->width() > 200);
        QVERIFY(status->height() >= status->fontMetrics().height());
        auto* dot = window.findChild<QLabel*>("connectionStatusDot"); QVERIFY(dot); QVERIFY(dot->text().isEmpty());
        QTemporaryDir temp;
        const auto path = temp.filePath("report.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"requestId", controller->requestId()}, {"files", QJsonArray{}}}).toJson());
        file.close();
        RemoteIMMessage message; message.fromUserId = "machine"; message.toUserId = "phone";
        message.createdAtMillis = 1; message.hasFile = true; message.file.localPath = path;
        message.file.fileName = RemoteDiagnostics::attachmentFileName(controller->requestId());
        // Same signal used after the production downloader completes, NOT the
        // unused incomingFile signal. Real network I/O is covered separately.
        emit fake->liveMessagesReceived({message});
        QTRY_VERIFY_WITH_TIMEOUT(!controller->isRunning(), 1500);
        QCOMPARE(fake->lastFilePeerId(), recipient);
        QVERIFY(status->isVisible()); QVERIFY(status->text().contains(recipient));
        QVERIFY(dot->text().isEmpty());
        QCOMPARE(app.chatState().selectedPeerId(), onlyPeer ? QString("machine") : QString("other"));
        QFile report(fake->lastFilePath()); QVERIFY(report.open(QIODevice::ReadOnly));
        QVERIFY(report.readAll().contains(controller->requestId().toUtf8())); report.close();
        QFile::remove(fake->lastFilePath());
        const auto screenshot = qEnvironmentVariable("MAICHAT_DIAGNOSTICS_UI_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(window.grab().save(screenshot + "-window.png"));
        }
    }
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void cancelFromTheRealMoreMenuDoesNotSend() { throughRealMenu(false); }
    void confirmFromTheRealMoreMenuSendsToTheLockedRecipientAndShowsStatus() { throughRealMenu(true); }
    void accountChangeDuringConfirmationDoesNotSend() { throughRealMenu(true, true); }
    void sendsMergedReportToTheCurrentPeer() { throughRealMenu(true, false, true); }
    void onlyFriendCanReceiveTheReport() { throughRealMenu(true, false, true, true); }
    void filteringOutTheSelectionDisablesConfirmation() {
        RemoteDiagnosticsDialog dialog("machine", {{"helper", "Same name", {}, {}}, {"other", "Same name", {}, {}}});
        dialog.show();
        auto* list = dialog.findChild<QListWidget*>("diagnosticsRecipients");
        auto* confirm = dialog.findChild<QPushButton*>("diagnosticsConfirm");
        auto* filter = dialog.findChild<QLineEdit*>("diagnosticsFilter");
        QVERIFY(list); QVERIFY(confirm); QVERIFY(filter); QVERIFY(!confirm->isEnabled());
        list->setCurrentRow(0); QVERIFY(confirm->isEnabled()); QVERIFY(list->item(0)->text().contains("helper"));
        filter->setText("other"); QVERIFY(!confirm->isEnabled());
        filter->clear(); QVERIFY(confirm->isEnabled());
    }
};
QTEST_MAIN(RemoteDiagnosticsUiTest)
#include "RemoteDiagnosticsUiTest.moc"
