#include "ui/RemoteDesktopSessionCard.h"

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

RemoteDesktopSessionCard::RemoteDesktopSessionCard(QString peerUserId, QWidget* parent)
    : QWidget(parent), peerUserId_(std::move(peerUserId)) {
    setObjectName(QStringLiteral("remoteSessionCard"));
    buildUi();
    applyStyle();

    elapsed_.start();
    refreshDuration();
    durationTimer_ = new QTimer(this);
    durationTimer_->setInterval(1000);
    connect(durationTimer_, &QTimer::timeout, this, &RemoteDesktopSessionCard::refreshDuration);
    durationTimer_->start();
}

void RemoteDesktopSessionCard::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("remoteCardHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 8, 14, 8);
    headerLayout->setSpacing(10);
    titleLabel_ = new QLabel(peerUserId_, header);
    titleLabel_->setObjectName(QStringLiteral("remoteCardTitle"));
    durationLabel_ = new QLabel(header);
    durationLabel_->setObjectName(QStringLiteral("remoteCardDuration"));
    headerLayout->addWidget(titleLabel_, 1);
    headerLayout->addWidget(durationLabel_);

    renderSurface_ = new QWidget(this);
    renderSurface_->setObjectName(QStringLiteral("remoteCardSurface"));
    // 需要独立原生句柄交给 TRTC；关掉 Qt 自身背景绘制，避免与 SDK 渲染互相覆盖。
    renderSurface_->setAttribute(Qt::WA_NativeWindow, true);
    renderSurface_->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    renderSurface_->setAttribute(Qt::WA_OpaquePaintEvent, true);
    renderSurface_->setAttribute(Qt::WA_StyledBackground, true);
    renderSurface_->setMinimumSize(UiZoom::s(320), UiZoom::s(180));

    auto* surfaceLayout = new QVBoxLayout(renderSurface_);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    placeholderLabel_ = new QLabel(QStringLiteral("正在连接对方屏幕…"), renderSurface_);
    placeholderLabel_->setObjectName(QStringLiteral("remoteCardPlaceholder"));
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    surfaceLayout->addWidget(placeholderLabel_);

    statusLabel_ = new QLabel(QStringLiteral("等待画面"), this);
    statusLabel_->setObjectName(QStringLiteral("remoteCardStatus"));

    layout->addWidget(header);
    layout->addWidget(renderSurface_, 1);
    layout->addWidget(statusLabel_);
}

void RemoteDesktopSessionCard::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #remoteCardHeader {
            background: #111c33;
        }
        #remoteCardTitle {
            color: #e2e8f0;
            font-size: 13px;
            font-weight: 700;
            background: transparent;
        }
        #remoteCardDuration {
            color: #94a3b8;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        #remoteCardSurface {
            background: #000000;
        }
        #remoteCardPlaceholder {
            color: #94a3b8;
            font-size: 14px;
            background: transparent;
        }
        #remoteCardStatus {
            background: #111c33;
            color: #64748b;
            font-size: 12px;
            padding: 5px 14px;
        }
    )")));
}

void RemoteDesktopSessionCard::setStreamActive(bool active) {
    streamActive_ = active;
    // 画面到达后必须撤下占位文字：它盖在渲染面上会挡住 SDK 画的内容。
    placeholderLabel_->setVisible(!active);
    setStatusText(active ? QStringLiteral("画面已连接") : QStringLiteral("等待画面"));
}

void RemoteDesktopSessionCard::setStatusText(const QString& text) {
    statusLabel_->setText(text);
}

void* RemoteDesktopSessionCard::renderWindowHandle() const {
    return reinterpret_cast<void*>(renderSurface_->winId());
}

QString RemoteDesktopSessionCard::statusText() const {
    return statusLabel_->text();
}

bool RemoteDesktopSessionCard::isStreamActive() const {
    return streamActive_;
}

void RemoteDesktopSessionCard::refreshDuration() {
    durationLabel_->setText(formatDuration(elapsed_.elapsed()));
}
