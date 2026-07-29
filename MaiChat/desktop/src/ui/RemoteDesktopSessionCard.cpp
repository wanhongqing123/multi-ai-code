#include "ui/RemoteDesktopSessionCard.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

namespace {

// 自绘全屏图标：Unicode 的 ⤢/⤡ 在 Windows 上落到 fallback 字体，细得几乎看不见。
// 按 DPR 出图并回标 devicePixelRatio，150% 缩放下才不发虚。
QPixmap fullScreenIcon(bool collapse, int logicalSize, const QColor& color, qreal dpr) {
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QPixmap pixmap(QSize(qRound(logicalSize * ratio), qRound(logicalSize * ratio)));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(ratio, ratio);
    QPen pen(color);
    pen.setWidthF(qMax(1.2, logicalSize / 11.0));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);

    // 四个直角括号。展开时顶在外框四角、开口朝内；收起时缩到内侧、开口朝外。
    const qreal outer = logicalSize * 0.12;
    const qreal inner = logicalSize * 0.38;
    const qreal far = logicalSize - outer;
    const qreal near = logicalSize - inner;
    const qreal cornerX[4] = {collapse ? inner : outer, collapse ? near : far,
                              collapse ? inner : outer, collapse ? near : far};
    const qreal cornerY[4] = {collapse ? inner : outer, collapse ? inner : outer,
                              collapse ? near : far, collapse ? near : far};
    // 每个角的两条臂各自朝哪边伸：展开朝内收，收起朝外张。
    const qreal armX[4] = {collapse ? outer : inner, collapse ? far : near,
                           collapse ? outer : inner, collapse ? far : near};
    const qreal armY[4] = {collapse ? outer : inner, collapse ? outer : inner,
                           collapse ? far : near, collapse ? far : near};
    for (int i = 0; i < 4; ++i) {
        painter.drawLine(QPointF(cornerX[i], cornerY[i]), QPointF(armX[i], cornerY[i]));
        painter.drawLine(QPointF(cornerX[i], cornerY[i]), QPointF(cornerX[i], armY[i]));
    }
    return pixmap;
}

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
    fullScreenButton_ = new QPushButton(header);
    fullScreenButton_->setObjectName(QStringLiteral("remoteCardFullScreen"));
    fullScreenButton_->setCursor(Qt::PointingHandCursor);
    fullScreenButton_->setFocusPolicy(Qt::NoFocus);
    connect(fullScreenButton_, &QPushButton::clicked, this,
            [this] { emit fullScreenToggleRequested(peerUserId_); });
    headerLayout->addWidget(titleLabel_, 1);
    headerLayout->addWidget(durationLabel_);
    headerLayout->addWidget(fullScreenButton_);

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

    // 画面区是原生窗口，双击不一定冒泡到卡片，这里直接盯住它自己的事件。
    // 顶栏那颗按钮是兜底入口：万一 SDK 在句柄上盖了自己的子窗口把鼠标吃掉，
    // 双击会失灵，但按钮永远点得到。
    renderSurface_->installEventFilter(this);
    placeholderLabel_->installEventFilter(this);
    header->installEventFilter(this);
    refreshFullScreenButton();
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
        #remoteCardFullScreen {
            background: transparent;
            border: none;
            color: #94a3b8;
            font-size: 15px;
            padding: 0 4px;
            min-width: 22px;
        }
        #remoteCardFullScreen:hover {
            color: #e2e8f0;
        }
    )")));
}

void RemoteDesktopSessionCard::refreshFullScreenButton() {
    const int size = UiZoom::s(16);
    fullScreenButton_->setIcon(QIcon(fullScreenIcon(fullScreenActive_, size,
                                                    QColor(QStringLiteral("#94a3b8")),
                                                    devicePixelRatioF())));
    fullScreenButton_->setIconSize(QSize(size, size));
    fullScreenButton_->setToolTip(fullScreenActive_
                                      ? QStringLiteral("退出全屏（双击画面或按 Esc）")
                                      : QStringLiteral("全屏（双击画面）"));
}

void RemoteDesktopSessionCard::setFullScreenActive(bool active) {
    fullScreenActive_ = active;
    refreshFullScreenButton();
}

bool RemoteDesktopSessionCard::isFullScreenActive() const {
    return fullScreenActive_;
}

void RemoteDesktopSessionCard::mouseDoubleClickEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    emit fullScreenToggleRequested(peerUserId_);
}

bool RemoteDesktopSessionCard::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        emit fullScreenToggleRequested(peerUserId_);
        return true;
    }
    return QWidget::eventFilter(watched, event);
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
