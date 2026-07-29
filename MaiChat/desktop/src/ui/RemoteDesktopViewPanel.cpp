#include "ui/RemoteDesktopViewPanel.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

namespace {

QString formatDuration(qint64 milliseconds) {
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2")
        .arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

RemoteDesktopViewPanel::RemoteDesktopViewPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("remoteDesktopViewPanel"));
    buildUi();
    applyStyle();

    durationTimer_ = new QTimer(this);
    durationTimer_->setInterval(1000);
    connect(durationTimer_, &QTimer::timeout, this, &RemoteDesktopViewPanel::refreshDuration);
    showIdle();
}

void RemoteDesktopViewPanel::buildUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    stack_ = new QStackedWidget(this);
    rootLayout->addWidget(stack_);

    // 空态：说明从哪里发起，避免用户点进来看到一片空白不知所措。
    idlePage_ = new QWidget(stack_);
    auto* idleLayout = new QVBoxLayout(idlePage_);
    idleLayout->setAlignment(Qt::AlignCenter);
    idleLayout->setSpacing(10);
    auto* idleTitle = new QLabel(QStringLiteral("没有进行中的远程桌面"), idlePage_);
    idleTitle->setObjectName(QStringLiteral("remoteIdleTitle"));
    idleTitle->setAlignment(Qt::AlignCenter);
    auto* idleHint = new QLabel(
        QStringLiteral("在会话里点顶部的显示器图标，即可查看对方屏幕"), idlePage_);
    idleHint->setObjectName(QStringLiteral("remoteIdleHint"));
    idleHint->setAlignment(Qt::AlignCenter);
    idleLayout->addWidget(idleTitle);
    idleLayout->addWidget(idleHint);
    stack_->addWidget(idlePage_);

    // 会话页：顶部信息条 + 画面 + 状态条。断开入口统一在聊天顶栏的三态按钮。
    sessionPage_ = new QWidget(stack_);
    auto* sessionLayout = new QVBoxLayout(sessionPage_);
    sessionLayout->setContentsMargins(0, 0, 0, 0);
    sessionLayout->setSpacing(0);

    auto* header = new QWidget(sessionPage_);
    header->setObjectName(QStringLiteral("remoteViewHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 10, 18, 10);
    headerLayout->setSpacing(12);
    titleLabel_ = new QLabel(header);
    titleLabel_->setObjectName(QStringLiteral("remoteViewTitle"));
    durationLabel_ = new QLabel(header);
    durationLabel_->setObjectName(QStringLiteral("remoteViewDuration"));
    headerLayout->addWidget(titleLabel_, 1);
    headerLayout->addWidget(durationLabel_);

    renderSurface_ = new QWidget(sessionPage_);
    renderSurface_->setObjectName(QStringLiteral("remoteViewSurface"));
    // 需要独立原生句柄交给 TRTC；关掉 Qt 自身背景绘制避免与 SDK 渲染互相覆盖。
    renderSurface_->setAttribute(Qt::WA_NativeWindow, true);
    renderSurface_->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    renderSurface_->setAttribute(Qt::WA_OpaquePaintEvent, true);
    renderSurface_->setAttribute(Qt::WA_StyledBackground, true);

    auto* surfaceLayout = new QVBoxLayout(renderSurface_);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    placeholderLabel_ = new QLabel(QStringLiteral("正在连接对方屏幕…"), renderSurface_);
    placeholderLabel_->setObjectName(QStringLiteral("remoteViewPlaceholder"));
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    surfaceLayout->addWidget(placeholderLabel_);

    statusLabel_ = new QLabel(QStringLiteral("等待画面"), sessionPage_);
    statusLabel_->setObjectName(QStringLiteral("remoteViewStatus"));

    sessionLayout->addWidget(header);
    sessionLayout->addWidget(renderSurface_, 1);
    sessionLayout->addWidget(statusLabel_);
    stack_->addWidget(sessionPage_);
}

void RemoteDesktopViewPanel::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #remoteDesktopViewPanel {
            background: #ffffff;
        }
        #remoteIdleTitle {
            color: #0f172a;
            font-size: 16px;
            font-weight: 800;
        }
        #remoteIdleHint {
            color: #94a3b8;
            font-size: 13px;
        }
        #remoteViewHeader {
            background: #111c33;
        }
        #remoteViewTitle {
            color: #e2e8f0;
            font-size: 14px;
            font-weight: 700;
            background: transparent;
        }
        #remoteViewDuration {
            color: #94a3b8;
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
        #remoteViewSurface {
            background: #000000;
        }
        #remoteViewPlaceholder {
            color: #94a3b8;
            font-size: 15px;
            background: transparent;
        }
        #remoteViewStatus {
            background: #111c33;
            color: #64748b;
            font-size: 12px;
            padding: 6px 18px;
        }
    )")));
}

void RemoteDesktopViewPanel::showIdle() {
    durationTimer_->stop();
    streamActive_ = false;
    stack_->setCurrentWidget(idlePage_);
}

void RemoteDesktopViewPanel::showConnecting(const QString& peerUserId) {
    titleLabel_->setText(QStringLiteral("远程桌面 · %1").arg(peerUserId));
    setStreamActive(false);
    elapsed_.start();
    refreshDuration();
    durationTimer_->start();
    stack_->setCurrentWidget(sessionPage_);
}

void RemoteDesktopViewPanel::setStreamActive(bool active) {
    streamActive_ = active;
    // 画面到达后必须撤下占位文字：它盖在渲染面上会挡住 SDK 画的内容。
    placeholderLabel_->setVisible(!active);
    setStatusText(active ? QStringLiteral("画面已连接") : QStringLiteral("等待画面"));
}

void RemoteDesktopViewPanel::setStatusText(const QString& text) {
    statusLabel_->setText(text);
}

void* RemoteDesktopViewPanel::renderWindowHandle() const {
    return reinterpret_cast<void*>(renderSurface_->winId());
}

QString RemoteDesktopViewPanel::statusText() const {
    return statusLabel_->text();
}

bool RemoteDesktopViewPanel::isStreamActive() const {
    return streamActive_;
}

bool RemoteDesktopViewPanel::isSessionVisible() const {
    return stack_->currentWidget() == sessionPage_;
}

void RemoteDesktopViewPanel::refreshDuration() {
    durationLabel_->setText(formatDuration(elapsed_.elapsed()));
}
