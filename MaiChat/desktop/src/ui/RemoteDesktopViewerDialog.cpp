#include "ui/RemoteDesktopViewerDialog.h"

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

RemoteDesktopViewerDialog::RemoteDesktopViewerDialog(const QString& peerUserId, QWidget* parent)
    : QDialog(parent) {
    buildUi(peerUserId);
    applyStyle();

    elapsed_.start();
    durationTimer_ = new QTimer(this);
    durationTimer_->setInterval(1000);
    connect(durationTimer_, &QTimer::timeout, this, &RemoteDesktopViewerDialog::refreshDuration);
    durationTimer_->start();
    refreshDuration();
}

void RemoteDesktopViewerDialog::buildUi(const QString& peerUserId) {
    setObjectName(QStringLiteral("remoteDesktopViewerDialog"));
    setWindowTitle(QStringLiteral("远程桌面 — %1").arg(peerUserId));
    // 非模态：看画面的同时还能操作主窗口（例如继续聊天）。
    setModal(false);
    setMinimumSize(UiZoom::s(720), UiZoom::s(480));
    resize(UiZoom::s(1120), UiZoom::s(700));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("viewerHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 10, 14, 10);
    headerLayout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("远程桌面 · %1").arg(peerUserId), header);
    title->setObjectName(QStringLiteral("viewerTitle"));

    durationLabel_ = new QLabel(header);
    durationLabel_->setObjectName(QStringLiteral("viewerDuration"));

    // 不放断开按钮：断开出口统一收敛到聊天顶栏那个三态按钮，
    // 关掉本窗口同样等同断开，窗内再放一个只是冗余。
    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(durationLabel_);

    // 渲染面：交给 TRTC 的原生窗口。开 WA_NativeWindow 保证有独立句柄，
    // 并关掉 Qt 自身绘制背景，避免与 SDK 的渲染互相覆盖导致闪烁。
    renderSurface_ = new QWidget(this);
    renderSurface_->setObjectName(QStringLiteral("viewerRenderSurface"));
    renderSurface_->setAttribute(Qt::WA_NativeWindow, true);
    renderSurface_->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    renderSurface_->setAttribute(Qt::WA_OpaquePaintEvent, true);
    renderSurface_->setAttribute(Qt::WA_StyledBackground, true);

    auto* surfaceLayout = new QVBoxLayout(renderSurface_);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    placeholderLabel_ = new QLabel(QStringLiteral("正在连接对方屏幕…"), renderSurface_);
    placeholderLabel_->setObjectName(QStringLiteral("viewerPlaceholder"));
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    surfaceLayout->addWidget(placeholderLabel_);

    statusLabel_ = new QLabel(QStringLiteral("等待画面"), this);
    statusLabel_->setObjectName(QStringLiteral("viewerStatus"));

    rootLayout->addWidget(header);
    rootLayout->addWidget(renderSurface_, 1);
    rootLayout->addWidget(statusLabel_);
}

void RemoteDesktopViewerDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #remoteDesktopViewerDialog {
            background: #0f172a;
        }
        #viewerHeader {
            background: #111c33;
        }
        #viewerTitle {
            color: #e2e8f0;
            font-size: 14px;
            font-weight: 700;
            background: transparent;
        }
        #viewerDuration {
            color: #94a3b8;
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
        #viewerRenderSurface {
            background: #000000;
        }
        #viewerPlaceholder {
            color: #94a3b8;
            font-size: 15px;
            background: transparent;
        }
        #viewerStatus {
            background: #111c33;
            color: #64748b;
            font-size: 12px;
            padding: 6px 18px;
        }
    )")));
}

void* RemoteDesktopViewerDialog::renderWindowHandle() const {
    return reinterpret_cast<void*>(renderSurface_->winId());
}

void RemoteDesktopViewerDialog::setStreamActive(bool active) {
    streamActive_ = active;
    // 画面到达后必须撤下占位文字：它盖在渲染面上会挡住 SDK 画的内容。
    placeholderLabel_->setVisible(!active);
    setStatusText(active ? QStringLiteral("画面已连接") : QStringLiteral("等待画面"));
}

void RemoteDesktopViewerDialog::setStatusText(const QString& text) {
    statusLabel_->setText(text);
}

QString RemoteDesktopViewerDialog::statusText() const {
    return statusLabel_->text();
}

bool RemoteDesktopViewerDialog::isStreamActive() const {
    return streamActive_;
}

void RemoteDesktopViewerDialog::refreshDuration() {
    durationLabel_->setText(formatDuration(elapsed_.elapsed()));
}
