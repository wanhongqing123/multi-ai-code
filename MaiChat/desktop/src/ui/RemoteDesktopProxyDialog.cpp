#include "ui/RemoteDesktopProxyDialog.h"

#include <QCheckBox>
#include <QColor>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

RemoteDesktopProxyDialog::RemoteDesktopProxyDialog(Config config, QWidget* parent)
    : QDialog(parent) {
    buildUi(config);
    applyStyle();
    updateFieldState();
}

RemoteDesktopProxyDialog::Config RemoteDesktopProxyDialog::config() const {
    Config result;
    result.enabled = enabled_->isChecked();
    result.host = host_->text().trimmed();
    result.port = static_cast<quint16>(port_->value());
    result.supportUdp = udp_->isChecked();
    return result;
}

void RemoteDesktopProxyDialog::buildUi(const Config& config) {
    setObjectName(QStringLiteral("remoteDesktopProxyDialog"));
    setWindowTitle(QStringLiteral("TRTC 网络代理"));
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("remoteDesktopProxyPanel"));
    auto* shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(UiZoom::s(32));
    shadow->setOffset(0, UiZoom::s(10));
    shadow->setColor(QColor(16, 24, 40, 42));
    panel->setGraphicsEffect(shadow);
    rootLayout->addWidget(panel);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(UiZoom::s(28), UiZoom::s(26),
                               UiZoom::s(28), UiZoom::s(22));
    layout->setSpacing(UiZoom::s(14));

    auto* title = new QLabel(QStringLiteral("TRTC 网络代理"), panel);
    title->setObjectName(QStringLiteral("remoteDesktopProxyTitle"));
    layout->addWidget(title);

    auto* description = new QLabel(
        QStringLiteral("为远程桌面单独设置 SOCKS5，不影响 IM。"
                       "代理会在 MaiChat 下次启动时生效。"),
        panel);
    description->setObjectName(QStringLiteral("remoteDesktopProxyDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);

    enabled_ = new QCheckBox(QStringLiteral("启用 SOCKS5 代理"), panel);
    enabled_->setObjectName(QStringLiteral("trtcProxyEnabled"));
    enabled_->setChecked(config.enabled);
    enabled_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(enabled_);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(UiZoom::s(16));
    form->setVerticalSpacing(UiZoom::s(12));
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    host_ = new QLineEdit(config.host, panel);
    host_->setObjectName(QStringLiteral("trtcProxyHost"));
    host_->setPlaceholderText(QStringLiteral("127.0.0.1"));
    host_->setClearButtonEnabled(true);

    port_ = new QSpinBox(panel);
    port_->setObjectName(QStringLiteral("trtcProxyPort"));
    port_->setRange(1, 65535);
    port_->setValue(config.port);

    udp_ = new QCheckBox(QStringLiteral("代理明确支持 UDP 时开启"), panel);
    udp_->setObjectName(QStringLiteral("trtcProxyUdp"));
    udp_->setChecked(config.supportUdp);
    udp_->setCursor(Qt::PointingHandCursor);

    auto* hostLabel = new QLabel(QStringLiteral("服务器"), panel);
    hostLabel->setObjectName(QStringLiteral("remoteDesktopProxyFieldLabel"));
    auto* portLabel = new QLabel(QStringLiteral("端口"), panel);
    portLabel->setObjectName(QStringLiteral("remoteDesktopProxyFieldLabel"));
    auto* transportLabel = new QLabel(QStringLiteral("传输"), panel);
    transportLabel->setObjectName(QStringLiteral("remoteDesktopProxyFieldLabel"));
    form->addRow(hostLabel, host_);
    form->addRow(portLabel, port_);
    form->addRow(transportLabel, udp_);
    layout->addLayout(form);

    error_ = new QLabel(panel);
    error_->setObjectName(QStringLiteral("remoteDesktopProxyError"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);

    auto* restartHint = new QLabel(
        QStringLiteral("保存后请完全退出并重新打开 MaiChat。仅关闭设置页不会重建 TRTC 实例。"),
        panel);
    restartHint->setObjectName(QStringLiteral("remoteDesktopProxyHint"));
    restartHint->setWordWrap(true);
    layout->addWidget(restartHint);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, UiZoom::s(2), 0, 0);
    buttonRow->setSpacing(UiZoom::s(10));
    buttonRow->addStretch(1);

    auto* cancel = new QPushButton(QStringLiteral("取消"), panel);
    cancel->setObjectName(QStringLiteral("remoteDesktopProxyCancel"));
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    auto* save = new QPushButton(QStringLiteral("保存"), panel);
    save->setObjectName(QStringLiteral("remoteDesktopProxySave"));
    save->setCursor(Qt::PointingHandCursor);
    save->setDefault(true);
    connect(save, &QPushButton::clicked, this,
            &RemoteDesktopProxyDialog::validateAndAccept);

    buttonRow->addWidget(cancel);
    buttonRow->addWidget(save);
    layout->addLayout(buttonRow);

    connect(enabled_, &QCheckBox::toggled, this,
            &RemoteDesktopProxyDialog::updateFieldState);
    connect(host_, &QLineEdit::textChanged, this, [this] {
        if (error_->isVisible()) error_->hide();
    });

    setMinimumWidth(UiZoom::s(500));
    setMaximumWidth(UiZoom::s(600));
}

void RemoteDesktopProxyDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #remoteDesktopProxyPanel {
            background: #ffffff;
            border-radius: 16px;
        }
        #remoteDesktopProxyTitle {
            color: #0f172a;
            font-size: 18px;
            font-weight: 800;
        }
        #remoteDesktopProxyDescription, #remoteDesktopProxyHint {
            color: #667085;
            font-size: 13px;
        }
        #remoteDesktopProxyFieldLabel {
            color: #334155;
            font-size: 14px;
            font-weight: 700;
        }
        #trtcProxyEnabled, #trtcProxyUdp {
            color: #172033;
            font-size: 14px;
            font-weight: 600;
            spacing: 8px;
        }
        #trtcProxyHost, #trtcProxyPort {
            min-height: 22px;
            background: #f8fafc;
            border: 1px solid #d9e1ec;
            border-radius: 10px;
            color: #172033;
            font-size: 14px;
            padding: 9px 12px;
        }
        #trtcProxyHost:focus, #trtcProxyPort:focus {
            background: #ffffff;
            border-color: #5b9bff;
        }
        #trtcProxyHost:disabled, #trtcProxyPort:disabled {
            background: #f1f5f9;
            color: #98a2b3;
        }
        #trtcProxyPort::up-button, #trtcProxyPort::down-button {
            width: 22px;
            background: #eef4ff;
            border: 0;
            border-left: 1px solid #d9e1ec;
        }
        #trtcProxyPort::up-button {
            subcontrol-origin: border;
            subcontrol-position: top right;
            border-top-right-radius: 9px;
        }
        #trtcProxyPort::down-button {
            subcontrol-origin: border;
            subcontrol-position: bottom right;
            border-bottom-right-radius: 9px;
        }
        #remoteDesktopProxyError {
            color: #b42318;
            font-size: 13px;
            font-weight: 700;
        }
        #remoteDesktopProxyCancel, #remoteDesktopProxySave {
            border-radius: 10px;
            font-size: 14px;
            font-weight: 700;
            padding: 10px 22px;
        }
        #remoteDesktopProxyCancel {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            color: #334155;
        }
        #remoteDesktopProxyCancel:hover {
            background: #e2e8f0;
        }
        #remoteDesktopProxySave {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #5b9bff, stop:1 #1e40af);
            border: 0;
            color: #ffffff;
        }
        #remoteDesktopProxySave:hover {
            background: #1e40af;
        }
    )")));
}

void RemoteDesktopProxyDialog::updateFieldState() {
    const bool enabled = enabled_->isChecked();
    host_->setEnabled(enabled);
    port_->setEnabled(enabled);
    udp_->setEnabled(enabled);
    if (!enabled) error_->hide();
}

void RemoteDesktopProxyDialog::validateAndAccept() {
    if (enabled_->isChecked() && host_->text().trimmed().isEmpty()) {
        error_->setText(QStringLiteral("请输入 SOCKS5 代理服务器地址。"));
        error_->show();
        host_->setFocus(Qt::OtherFocusReason);
        return;
    }
    accept();
}
