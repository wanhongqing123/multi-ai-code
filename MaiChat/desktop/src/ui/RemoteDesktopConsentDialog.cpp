#include "ui/RemoteDesktopConsentDialog.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

RemoteDesktopConsentDialog::RemoteDesktopConsentDialog(const QString& fromUserId,
                                                       int timeoutMs,
                                                       QWidget* parent)
    : QDialog(parent), remainingSeconds_(qMax(1, timeoutMs / 1000)) {
    buildUi(fromUserId);
    applyStyle();
    updateCountdownLabel();

    countdownTimer_ = new QTimer(this);
    countdownTimer_->setInterval(1000);
    connect(countdownTimer_, &QTimer::timeout, this, &RemoteDesktopConsentDialog::handleTick);
    countdownTimer_->start();
}

int RemoteDesktopConsentDialog::remainingSeconds() const {
    return remainingSeconds_;
}

void RemoteDesktopConsentDialog::tickForTest() {
    handleTick();
}

void RemoteDesktopConsentDialog::buildUi(const QString& fromUserId) {
    setObjectName(QStringLiteral("remoteDesktopConsentDialog"));
    setWindowTitle(QStringLiteral("远程桌面请求"));
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(UiZoom::s(520), UiZoom::s(300));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("remoteDesktopConsentPanel"));
    auto* shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(32);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(16, 24, 40, 38));
    panel->setGraphicsEffect(shadow);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(26, 24, 26, 22);
    panelLayout->setSpacing(0);

    auto* title = new QLabel(QStringLiteral("远程桌面请求"), panel);
    title->setObjectName(QStringLiteral("remoteDesktopConsentTitle"));

    auto* body = new QLabel(panel);
    body->setObjectName(QStringLiteral("remoteDesktopConsentBody"));
    body->setWordWrap(true);
    // 明确写出后果：用户要清楚"同意"意味着对方能实时看到整个屏幕。
    body->setText(QStringLiteral("%1 请求查看你的屏幕。\n同意后对方将实时看到你的主屏画面，你可以随时停止共享。")
                      .arg(fromUserId));

    countdownLabel_ = new QLabel(panel);
    countdownLabel_->setObjectName(QStringLiteral("remoteDesktopConsentCountdown"));

    rejectButton_ = new QPushButton(QStringLiteral("拒绝"), panel);
    rejectButton_->setObjectName(QStringLiteral("remoteDesktopConsentReject"));
    rejectButton_->setCursor(Qt::PointingHandCursor);
    // 默认按钮是「拒绝」：误按回车时应当拒绝而不是把屏幕共享出去。
    rejectButton_->setDefault(true);

    allowButton_ = new QPushButton(QStringLiteral("允许查看"), panel);
    allowButton_->setObjectName(QStringLiteral("remoteDesktopConsentAllow"));
    allowButton_->setCursor(Qt::PointingHandCursor);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(12);
    buttonRow->addStretch(1);
    buttonRow->addWidget(rejectButton_);
    buttonRow->addWidget(allowButton_);

    panelLayout->addWidget(title);
    panelLayout->addSpacing(UiZoom::s(12));
    panelLayout->addWidget(body);
    panelLayout->addSpacing(UiZoom::s(14));
    panelLayout->addWidget(countdownLabel_);
    panelLayout->addStretch(1);
    panelLayout->addLayout(buttonRow);

    rootLayout->addWidget(panel);

    connect(rejectButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(allowButton_, &QPushButton::clicked, this, &QDialog::accept);
}

void RemoteDesktopConsentDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #remoteDesktopConsentPanel {
            background: #ffffff;
            border-radius: 16px;
        }
        #remoteDesktopConsentTitle {
            color: #0f172a;
            font-size: 18px;
            font-weight: 800;
        }
        #remoteDesktopConsentBody {
            color: #334155;
            font-size: 14px;
            line-height: 22px;
        }
        #remoteDesktopConsentCountdown {
            color: #b42318;
            font-size: 13px;
            font-weight: 700;
        }
        #remoteDesktopConsentReject, #remoteDesktopConsentAllow {
            border-radius: 10px;
            font-size: 14px;
            font-weight: 700;
            padding: 10px 22px;
        }
        #remoteDesktopConsentReject {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            color: #334155;
        }
        #remoteDesktopConsentReject:hover {
            background: #e2e8f0;
        }
        #remoteDesktopConsentAllow {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #5b9bff, stop:1 #1e40af);
            border: 0;
            color: #ffffff;
        }
        #remoteDesktopConsentAllow:hover {
            background: #1e40af;
        }
    )")));
}

void RemoteDesktopConsentDialog::updateCountdownLabel() {
    countdownLabel_->setText(QStringLiteral("%1 秒后自动拒绝").arg(remainingSeconds_));
}

void RemoteDesktopConsentDialog::handleTick() {
    remainingSeconds_ -= 1;
    if (remainingSeconds_ <= 0) {
        remainingSeconds_ = 0;
        updateCountdownLabel();
        if (countdownTimer_) countdownTimer_->stop();
        // 超时按拒绝处理：无人应答时保持收紧。
        reject();
        return;
    }
    updateCountdownLabel();
}
