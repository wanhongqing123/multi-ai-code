// 文件作用：排队到 Qt 主线程显示两次 Password 输入，工作线程等待自己的安全结果。
// 密码不进入 tool JSON、question、审批、诊断或设置；不嵌套主线程事件循环。
#include "MaiChatTools/DesktopPdfPasswordPrompt.h"

#include <QApplication>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "ui/UiZoom.h"

namespace {

struct PromptState {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<MaiResult<std::string>> result;
    bool abandoned = false;
    QPointer<QDialog> dialog; // 只在 UI 线程读写，不让等待线程操作控件。
};

void finish(const std::shared_ptr<PromptState>& state, MaiResult<std::string> result) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->result || state->abandoned) return;
    state->result.emplace(std::move(result));
    state->changed.notify_all();
}

void showPrompt(const std::shared_ptr<PromptState>& state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->abandoned) return; // 取消先于排队展示完成时，不出现迟到的弹窗。
    }
    QWidget* parent = QApplication::activeWindow();
    if (!qApp || qApp->applicationState() != Qt::ApplicationActive || !parent ||
        !parent->isVisible() || QApplication::activeModalWidget()) {
        finish(state, {MaiErrorCode::NotConfigured, "the PDF password form needs the active application window"});
        return;
    }
    auto* dialog = new QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint);
    state->dialog = dialog;
    dialog->setObjectName(QStringLiteral("pdfPasswordDialog"));
    dialog->setWindowTitle(QStringLiteral("设置 PDF 打开密码"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAttribute(Qt::WA_TranslucentBackground);
    dialog->setMinimumWidth(UiZoom::s(420));
    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(18, 18, 18, 18);
    auto* panel = new QFrame(dialog);
    panel->setObjectName(QStringLiteral("pdfPasswordPanel"));
    root->addWidget(panel);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(28, 26, 28, 24);
    layout->setSpacing(UiZoom::s(12));
    auto* title = new QLabel(QStringLiteral("设置 PDF 打开密码"), panel);
    title->setObjectName(QStringLiteral("pdfPasswordTitle"));
    layout->addWidget(title);
    auto* description = new QLabel(QStringLiteral("为新生成的 PDF 设置打开密码，再输入一次确认。密码最多 127 字节，不会自动去除空格。"), panel);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* password = new QLineEdit(panel);
    auto* confirmation = new QLineEdit(panel);
    password->setObjectName(QStringLiteral("pdfPasswordNew"));
    confirmation->setObjectName(QStringLiteral("pdfPasswordConfirm"));
    password->setPlaceholderText(QStringLiteral("PDF 打开密码"));
    confirmation->setPlaceholderText(QStringLiteral("再次输入密码"));
    for (QLineEdit* field : {password, confirmation}) {
        field->setEchoMode(QLineEdit::Password);
        field->setMaxLength(254);
        field->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
        field->setClearButtonEnabled(false);
        layout->addWidget(field);
    }
    auto* error = new QLabel(panel);
    error->setObjectName(QStringLiteral("pdfPasswordError"));
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* cancel = new QPushButton(QStringLiteral("取消"), panel);
    auto* confirm = new QPushButton(QStringLiteral("保护 PDF"), panel);
    cancel->setObjectName(QStringLiteral("pdfPasswordCancel"));
    confirm->setObjectName(QStringLiteral("pdfPasswordSave"));
    for (QPushButton* button : {cancel, confirm}) button->setMinimumSize(UiZoom::s(100), UiZoom::s(40));
    buttons->addWidget(cancel);
    buttons->addWidget(confirm);
    layout->addLayout(buttons);
    dialog->setStyleSheet(QStringLiteral(
        "#pdfPasswordPanel{background:#fff;border:1px solid #dbe5f1;border-radius:16px;}"
        "QLabel{color:#23324a;}#pdfPasswordTitle{font-size:20px;font-weight:600;}"
        "QLineEdit{min-height:38px;padding:0 10px;border:1px solid #d5e0ee;border-radius:8px;background:#f8fbff;}"
        "QLineEdit:focus{border:1px solid #168ee0;}"
        "QPushButton{min-height:36px;border:1px solid #d5e0ee;border-radius:8px;background:#f1f6fc;}"
        "#pdfPasswordSave{background:#168ee0;color:white;border:0;}#pdfPasswordError{color:#c43236;}"));
    QObject::connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    auto accept = [state, dialog, password, confirmation, error] {
        const QByteArray bytes = password->text().toUtf8();
        if (bytes.isEmpty() || bytes.size() > 127 || bytes.contains('\0')) {
            error->setText(QStringLiteral("请输入非空密码，最多 127 字节，不能包含空字符。"));
            error->show();
            return;
        }
        if (password->text() != confirmation->text()) {
            error->setText(QStringLiteral("两次输入的密码不一致。"));
            error->show();
            return;
        }
        finish(state, std::string(bytes.constData(), size_t(bytes.size())));
        password->clear();
        confirmation->clear();
        dialog->accept();
    };
    QObject::connect(confirm, &QPushButton::clicked, dialog, accept);
    QObject::connect(confirmation, &QLineEdit::returnPressed, dialog, accept);
    QObject::connect(dialog, &QDialog::finished, dialog, [state, password, confirmation](int) {
        password->clear(); confirmation->clear();
        finish(state, {MaiErrorCode::Canceled, "PDF password input was canceled"});
    });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, dialog, [state, dialog] {
        finish(state, {MaiErrorCode::Canceled, "PDF password input was canceled"});
        dialog->reject();
    });
    auto* timer = new QTimer(dialog);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, dialog, [state, dialog] {
        bool abandoned;
        { std::lock_guard<std::mutex> lock(state->mutex); abandoned = state->abandoned; }
        if (abandoned) dialog->reject();
    });
    timer->start();
    dialog->show();
    password->setFocus();
}

} // namespace

MaiResult<std::string> requestDesktopPdfPassword(const std::atomic<bool>* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        return {MaiErrorCode::Canceled, "PDF password input was canceled"};
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!application)
        return {MaiErrorCode::NotConfigured, "the PDF password form is unavailable"};
    if (QThread::currentThread() == application->thread())
        return {MaiErrorCode::InvalidInput, "PDF password input must run on a worker thread"};
    auto state = std::make_shared<PromptState>();
    if (!QMetaObject::invokeMethod(application, [state] { showPrompt(state); }, Qt::QueuedConnection))
        return {MaiErrorCode::NotConfigured, "the PDF password form is unavailable"};
    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        if ((cancel && cancel->load(std::memory_order_relaxed)) || QCoreApplication::closingDown()) {
            state->abandoned = true;
            state->result.reset();
            lock.unlock();
            // UI 回调只持有本次 state，不读取 worker 的 cancel 指针或关闭别的表单。
            QMetaObject::invokeMethod(application, [state] {
                if (state->dialog) state->dialog->reject();
            }, Qt::QueuedConnection);
            return {MaiErrorCode::Canceled, "PDF password input was canceled"};
        }
        if (state->result) return std::move(*state->result);
        state->changed.wait_for(lock, std::chrono::milliseconds(50));
    }
}
