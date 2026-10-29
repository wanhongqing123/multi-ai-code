// 文件作用：私有 PDF 密码输入的工作线程/UI 取消契约；不生成真实账号密码或工具参数。
#include <QtTest>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <QWindow>
#include <atomic>
#include <optional>
#include <thread>

#include "MaiChatTools/DesktopPdfPasswordPrompt.h"

#ifdef Q_OS_MACOS
void activateDesktopPasswordTestApplication();
#endif

namespace {
struct JoinPrompt {
    std::atomic<bool>& cancel;
    std::thread& worker;
    ~JoinPrompt() {
        cancel.store(true);
        if (worker.joinable()) worker.join();
    }
};
QDialog* visiblePrompt(QWidget& window) {
    for (auto* dialog : window.findChildren<QDialog*>(QStringLiteral("pdfPasswordDialog")))
        if (dialog->isVisible()) return dialog;
    return nullptr;
}
}

class DesktopPdfPasswordPromptTest : public QObject {
    Q_OBJECT
private slots:
    void rejectsMainThreadAndPreCanceledInput();
    void workerInputAndCancelUseOnlyTheOwnedNativeForm();
};

void DesktopPdfPasswordPromptTest::rejectsMainThreadAndPreCanceledInput() {
    const auto wrongThread = requestDesktopPdfPassword(nullptr);
    QVERIFY(!wrongThread.isOk());
    QCOMPARE(wrongThread.error().code(), MaiErrorCode::InvalidInput);
    std::atomic<bool> canceled{true};
    const auto beforeOpen = requestDesktopPdfPassword(&canceled);
    QVERIFY(!beforeOpen.isOk());
    QCOMPARE(beforeOpen.error().code(), MaiErrorCode::Canceled);
}

void DesktopPdfPasswordPromptTest::workerInputAndCancelUseOnlyTheOwnedNativeForm() {
    QWidget window;
    window.show();
#ifdef Q_OS_MACOS
    activateDesktopPasswordTestApplication();
#endif
    window.raise(); window.activateWindow();
    if (window.windowHandle()) window.windowHandle()->requestActivate();
    QTRY_COMPARE_WITH_TIMEOUT(qApp->applicationState(), Qt::ApplicationActive, 3000);
    std::atomic<bool> cancel{false}, done{false};
    std::optional<MaiResult<std::string>> result;
    std::thread worker([&] { result = requestDesktopPdfPassword(&cancel); done.store(true); });
    JoinPrompt join{cancel, worker};
    QTRY_VERIFY_WITH_TIMEOUT(visiblePrompt(window) != nullptr, 3000);
    auto* dialog = visiblePrompt(window);
    auto* password = dialog->findChild<QLineEdit*>(QStringLiteral("pdfPasswordNew"));
    auto* confirmation = dialog->findChild<QLineEdit*>(QStringLiteral("pdfPasswordConfirm"));
    QVERIFY(password && confirmation);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    QCOMPARE(confirmation->echoMode(), QLineEdit::Password);
    const QString capture = qEnvironmentVariable("MAICHAT_PDF_PASSWORD_FORM_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(dialog->grab().save(capture));
    password->setText(QStringLiteral(" PDF fixture secret "));
    confirmation->setText(password->text());
    dialog->findChild<QPushButton*>(QStringLiteral("pdfPasswordSave"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(done.load(), 3000);
    worker.join();
    QVERIFY(result && result->isOk());
    QVERIFY(result->value() == " PDF fixture secret "); // 前后空格不被静默 trim。

    cancel.store(false); done.store(false); result.reset();
    worker = std::thread([&] { result = requestDesktopPdfPassword(&cancel); done.store(true); });
    QTRY_VERIFY_WITH_TIMEOUT(visiblePrompt(window) != nullptr, 3000);
    cancel.store(true);
    QTRY_VERIFY_WITH_TIMEOUT(done.load(), 3000);
    worker.join();
    QVERIFY(result && !result->isOk());
    QCOMPARE(result->error().code(), MaiErrorCode::Canceled);
    QTRY_VERIFY_WITH_TIMEOUT(visiblePrompt(window) == nullptr, 3000);
    QVERIFY(window.isVisible());
}

QTEST_MAIN(DesktopPdfPasswordPromptTest)
#include "DesktopPdfPasswordPromptTest.moc"
