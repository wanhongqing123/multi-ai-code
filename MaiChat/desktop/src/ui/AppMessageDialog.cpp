#include "ui/AppMessageDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

AppMessageDialog::AppMessageDialog(Options options, QWidget* parent) : QDialog(parent) {
    buildUi(options);
    applyStyle();
}

void AppMessageDialog::show(QWidget* parent, Kind kind, const QString& title,
                            const QString& body) {
    Options options;
    options.kind = kind;
    options.title = title;
    options.body = body;
    AppMessageDialog dialog(options, parent);
    dialog.exec();
}

bool AppMessageDialog::confirm(QWidget* parent, const QString& title, const QString& body,
                               const QString& confirmText, bool destructive) {
    Options options;
    options.kind = Kind::Confirm;
    options.title = title;
    options.body = body;
    options.confirmText = confirmText;
    options.destructive = destructive;
    AppMessageDialog dialog(options, parent);
    return dialog.exec() == QDialog::Accepted;
}

void AppMessageDialog::buildUi(const Options& options) {
    setObjectName(QStringLiteral("appMessageDialog"));
    setWindowTitle(options.title);
    setModal(true);
    // 去掉系统标题栏，标题改由面板内的标签承担（与 AppTextInputDialog 一致）。
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("appMessagePanel"));
    rootLayout->addWidget(panel);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(28, 26, 28, 22);
    layout->setSpacing(UiZoom::s(12));

    auto* title = new QLabel(options.title, panel);
    title->setObjectName(options.kind == Kind::Warning
                             ? QStringLiteral("appMessageTitleWarning")
                             : QStringLiteral("appMessageTitle"));
    layout->addWidget(title);

    auto* body = new QLabel(options.body, panel);
    body->setObjectName(QStringLiteral("appMessageBody"));
    body->setWordWrap(true);
    // 路径、错误详情之类需要能选中复制出来。
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(body);

    layout->addSpacing(UiZoom::s(4));

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(UiZoom::s(10));
    buttonRow->addStretch(1);

    if (options.kind == Kind::Confirm) {
        auto* cancel = new QPushButton(options.cancelText, panel);
        cancel->setObjectName(QStringLiteral("appMessageCancel"));
        cancel->setCursor(Qt::PointingHandCursor);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        buttonRow->addWidget(cancel);

        auto* confirm = new QPushButton(options.confirmText, panel);
        confirm->setObjectName(options.destructive ? QStringLiteral("appMessageDanger")
                                                   : QStringLiteral("appMessageConfirm"));
        confirm->setCursor(Qt::PointingHandCursor);
        connect(confirm, &QPushButton::clicked, this, &QDialog::accept);
        buttonRow->addWidget(confirm);

        // 危险动作把默认焦点留给取消：误按回车不该直接删掉东西。
        if (options.destructive) {
            cancel->setDefault(true);
        } else {
            confirm->setDefault(true);
        }
    } else {
        auto* close = new QPushButton(QStringLiteral("知道了"), panel);
        close->setObjectName(QStringLiteral("appMessageConfirm"));
        close->setCursor(Qt::PointingHandCursor);
        close->setDefault(true);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        buttonRow->addWidget(close);
    }

    layout->addLayout(buttonRow);
    setMinimumWidth(UiZoom::s(440));
    setMaximumWidth(UiZoom::s(620));
}

void AppMessageDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #appMessagePanel {
            background: #ffffff;
            border-radius: 16px;
        }
        #appMessageTitle, #appMessageTitleWarning {
            font-size: 18px;
            font-weight: 800;
        }
        #appMessageTitle {
            color: #0f172a;
        }
        #appMessageTitleWarning {
            color: #b42318;
        }
        #appMessageBody {
            color: #334155;
            font-size: 14px;
            line-height: 22px;
        }
        #appMessageCancel, #appMessageConfirm, #appMessageDanger {
            border-radius: 10px;
            font-size: 14px;
            font-weight: 700;
            padding: 10px 22px;
        }
        #appMessageCancel {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            color: #334155;
        }
        #appMessageCancel:hover {
            background: #e2e8f0;
        }
        #appMessageConfirm {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #5b9bff, stop:1 #1e40af);
            border: 0;
            color: #ffffff;
        }
        #appMessageConfirm:hover {
            background: #1e40af;
        }
        #appMessageDanger {
            background: #b42318;
            border: 0;
            color: #ffffff;
        }
        #appMessageDanger:hover {
            background: #912018;
        }
    )")));
}
