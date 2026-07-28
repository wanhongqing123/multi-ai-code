#include "ui/SharingIndicatorBar.h"

#include <QHBoxLayout>

#include "ui/UiZoom.h"

namespace {

QString formatElapsed(qint64 milliseconds) {
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds / 1000);
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

}  // namespace

SharingIndicatorBar::SharingIndicatorBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sharingIndicatorBar"));
    // QWidget 子类默认不绘制样式表背景（只有 QFrame 等内建类会）。不开这个属性，
    // 红底不会被画出来，白字落在白底上 = 指示条肉眼不可见。
    setAttribute(Qt::WA_StyledBackground, true);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(20, 8, 16, 8);
    layout->setSpacing(12);

    textLabel_ = new QLabel(this);
    textLabel_->setObjectName(QStringLiteral("sharingIndicatorText"));

    stopButton_ = new QPushButton(QStringLiteral("停止共享"), this);
    stopButton_->setObjectName(QStringLiteral("sharingIndicatorStop"));
    stopButton_->setCursor(Qt::PointingHandCursor);

    layout->addWidget(textLabel_, 1);
    layout->addWidget(stopButton_);

    applyStyle();
    setVisible(false);

    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(1000);
    connect(tickTimer_, &QTimer::timeout, this, &SharingIndicatorBar::refreshText);
    connect(stopButton_, &QPushButton::clicked, this, &SharingIndicatorBar::stopRequested);
}

void SharingIndicatorBar::startSharing(const QString& peerUserId) {
    peerUserId_ = peerUserId;
    elapsed_.start();
    refreshText();
    setVisible(true);
    tickTimer_->start();
}

void SharingIndicatorBar::stopSharing() {
    tickTimer_->stop();
    setVisible(false);
    peerUserId_.clear();
}

QString SharingIndicatorBar::currentText() const {
    return textLabel_->text();
}

void SharingIndicatorBar::refreshText() {
    textLabel_->setText(QStringLiteral("● 正在共享屏幕给 %1 · %2")
                            .arg(peerUserId_, formatElapsed(elapsed_.elapsed())));
}

void SharingIndicatorBar::applyStyle() {
    // 红底白字：与应用其余部分的蓝色体系形成强对比，避免被当成普通提示忽略。
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #sharingIndicatorBar {
            background: #b42318;
        }
        #sharingIndicatorText {
            color: #ffffff;
            font-size: 13px;
            font-weight: 700;
            background: transparent;
        }
        #sharingIndicatorStop {
            background: rgba(255, 255, 255, 0.18);
            border: 1px solid rgba(255, 255, 255, 0.45);
            border-radius: 8px;
            color: #ffffff;
            font-size: 12px;
            font-weight: 700;
            padding: 5px 14px;
        }
        #sharingIndicatorStop:hover {
            background: rgba(255, 255, 255, 0.32);
        }
    )")));
}
