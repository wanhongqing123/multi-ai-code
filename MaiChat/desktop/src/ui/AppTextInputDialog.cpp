#include "ui/AppTextInputDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

AppTextInputDialog::AppTextInputDialog(Options options, QWidget* parent) : QDialog(parent) {
    buildUi(options);
    applyStyle();
}

QString AppTextInputDialog::text() const {
    return input_->text();
}

QString AppTextInputDialog::getText(QWidget* parent, const Options& options, bool* accepted) {
    AppTextInputDialog dialog(options, parent);
    const bool ok = dialog.exec() == QDialog::Accepted;
    if (accepted != nullptr) *accepted = ok;
    return ok ? dialog.text() : QString();
}

void AppTextInputDialog::buildUi(const Options& options) {
    setObjectName(QStringLiteral("appTextInputDialog"));
    setWindowTitle(options.title);
    setModal(true);
    // 去掉系统标题栏：原生标题栏会带一个「?」帮助按钮，和这套界面不搭，
    // 标题改由面板内部的标签承担。
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("appTextInputPanel"));
    rootLayout->addWidget(panel);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(28, 26, 28, 22);
    layout->setSpacing(UiZoom::s(12));

    auto* title = new QLabel(options.title, panel);
    title->setObjectName(QStringLiteral("appTextInputTitle"));
    layout->addWidget(title);

    if (!options.description.isEmpty()) {
        auto* description = new QLabel(options.description, panel);
        description->setObjectName(QStringLiteral("appTextInputDescription"));
        description->setWordWrap(true);
        layout->addWidget(description);
    }

    input_ = new QLineEdit(options.initialText, panel);
    input_->setObjectName(QStringLiteral("appTextInput"));
    input_->setPlaceholderText(options.placeholder);
    if (options.password) input_->setEchoMode(QLineEdit::Password);
    input_->setClearButtonEnabled(!options.password);
    layout->addWidget(input_);

    layout->addSpacing(UiZoom::s(4));

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(UiZoom::s(10));
    buttonRow->addStretch(1);

    auto* cancel = new QPushButton(options.cancelText, panel);
    cancel->setObjectName(QStringLiteral("appTextInputCancel"));
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    auto* confirm = new QPushButton(options.confirmText, panel);
    confirm->setObjectName(QStringLiteral("appTextInputConfirm"));
    confirm->setCursor(Qt::PointingHandCursor);
    // 输入框里按回车等同于点确定，省一次鼠标往返。
    confirm->setDefault(true);
    connect(confirm, &QPushButton::clicked, this, &QDialog::accept);
    connect(input_, &QLineEdit::returnPressed, this, &QDialog::accept);

    buttonRow->addWidget(cancel);
    buttonRow->addWidget(confirm);
    layout->addLayout(buttonRow);

    setMinimumWidth(UiZoom::s(460));
    input_->setFocus(Qt::OtherFocusReason);
}

void AppTextInputDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #appTextInputPanel {
            background: #ffffff;
            border-radius: 16px;
        }
        #appTextInputTitle {
            color: #0f172a;
            font-size: 18px;
            font-weight: 800;
        }
        #appTextInputDescription {
            color: #667085;
            font-size: 13px;
        }
        #appTextInput {
            background: #f8fafc;
            border: 1px solid #d9e1ec;
            border-radius: 10px;
            color: #172033;
            font-size: 14px;
            padding: 10px 12px;
        }
        #appTextInput:focus {
            background: #ffffff;
            border-color: #5b9bff;
        }
        #appTextInputCancel, #appTextInputConfirm {
            border-radius: 10px;
            font-size: 14px;
            font-weight: 700;
            padding: 10px 22px;
        }
        #appTextInputCancel {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            color: #334155;
        }
        #appTextInputCancel:hover {
            background: #e2e8f0;
        }
        #appTextInputConfirm {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #5b9bff, stop:1 #1e40af);
            border: 0;
            color: #ffffff;
        }
        #appTextInputConfirm:hover {
            background: #1e40af;
        }
    )")));
}
