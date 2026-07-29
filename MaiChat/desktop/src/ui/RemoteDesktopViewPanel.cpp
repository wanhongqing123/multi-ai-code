#include "ui/RemoteDesktopViewPanel.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QtMath>

#include "ui/RemoteDesktopSessionCard.h"
#include "ui/UiZoom.h"

RemoteDesktopViewPanel::RemoteDesktopViewPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("remoteDesktopViewPanel"));
    buildUi();
    applyStyle();
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

    // 画面页：卡片网格。断开入口统一在聊天顶栏的三态按钮，这里不放按钮。
    gridPage_ = new QWidget(stack_);
    gridLayout_ = new QGridLayout(gridPage_);
    gridLayout_->setContentsMargins(UiZoom::s(12), UiZoom::s(12), UiZoom::s(12), UiZoom::s(12));
    gridLayout_->setSpacing(UiZoom::s(12));
    stack_->addWidget(gridPage_);
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
    )")));
}

void RemoteDesktopViewPanel::showIdle() {
    for (auto* card : cards_) {
        gridLayout_->removeWidget(card);
        card->deleteLater();
    }
    cards_.clear();
    order_.clear();
    stack_->setCurrentWidget(idlePage_);
}

void RemoteDesktopViewPanel::beginSession(const QString& peerUserId) {
    if (peerUserId.isEmpty()) return;
    if (!cards_.contains(peerUserId)) {
        auto* card = new RemoteDesktopSessionCard(peerUserId, gridPage_);
        cards_.insert(peerUserId, card);
        order_.append(peerUserId);
        relayoutCards();
    }
    stack_->setCurrentWidget(gridPage_);
}

void RemoteDesktopViewPanel::endSession(const QString& peerUserId) {
    auto* card = cards_.take(peerUserId);
    if (card == nullptr) return;
    order_.removeAll(peerUserId);
    gridLayout_->removeWidget(card);
    card->deleteLater();
    relayoutCards();
    // 最后一路结束就回空态，否则用户会对着一块黑屏以为还连着。
    if (cards_.isEmpty()) stack_->setCurrentWidget(idlePage_);
}

void RemoteDesktopViewPanel::relayoutCards() {
    for (auto* card : cards_) gridLayout_->removeWidget(card);
    for (int column = 0; column < gridLayout_->columnCount(); ++column) {
        gridLayout_->setColumnStretch(column, 0);
    }
    for (int row = 0; row < gridLayout_->rowCount(); ++row) {
        gridLayout_->setRowStretch(row, 0);
    }

    // 1 路占满，2 路并排，3~4 路两列，再多按平方根扩列。
    const int count = order_.size();
    const int columns = count <= 1 ? 1 : qCeil(qSqrt(static_cast<qreal>(count)));
    for (int index = 0; index < count; ++index) {
        auto* card = cards_.value(order_.at(index));
        if (card == nullptr) continue;
        const int row = index / columns;
        const int column = index % columns;
        gridLayout_->addWidget(card, row, column);
        gridLayout_->setColumnStretch(column, 1);
        gridLayout_->setRowStretch(row, 1);
    }
}

RemoteDesktopSessionCard* RemoteDesktopViewPanel::cardFor(const QString& peerUserId) const {
    return cards_.value(peerUserId, nullptr);
}

RemoteDesktopSessionCard* RemoteDesktopViewPanel::soleCard() const {
    return order_.size() == 1 ? cards_.value(order_.first(), nullptr) : nullptr;
}

void RemoteDesktopViewPanel::setStreamActive(const QString& peerUserId, bool active) {
    auto* card = cardFor(peerUserId);
    // TRTC 侧的 userId 理论上等于 IM userId；万一对不上而此刻只有一路，
    // 就落到那一路上 —— 宁可画面照常出来，也不要退回黑屏。
    if (card == nullptr) card = soleCard();
    if (card != nullptr) card->setStreamActive(active);
}

void RemoteDesktopViewPanel::setStatusText(const QString& peerUserId, const QString& text) {
    auto* card = cardFor(peerUserId);
    if (card == nullptr) card = soleCard();
    if (card != nullptr) card->setStatusText(text);
}

void* RemoteDesktopViewPanel::renderWindowHandle(const QString& peerUserId) const {
    auto* card = cardFor(peerUserId);
    if (card == nullptr) card = soleCard();
    return card != nullptr ? card->renderWindowHandle() : nullptr;
}

void RemoteDesktopViewPanel::setStreamActive(bool active) {
    if (auto* card = soleCard()) card->setStreamActive(active);
}

void RemoteDesktopViewPanel::setStatusText(const QString& text) {
    // 出错提示要让每一路都看见，不能只落在第一张卡片上。
    for (auto* card : cards_) card->setStatusText(text);
}

void* RemoteDesktopViewPanel::renderWindowHandle() const {
    auto* card = soleCard();
    return card != nullptr ? card->renderWindowHandle() : nullptr;
}

QString RemoteDesktopViewPanel::statusText() const {
    auto* card = soleCard();
    return card != nullptr ? card->statusText() : QString();
}

bool RemoteDesktopViewPanel::isStreamActive() const {
    auto* card = soleCard();
    return card != nullptr && card->isStreamActive();
}

bool RemoteDesktopViewPanel::isSessionVisible() const {
    return stack_->currentWidget() == gridPage_;
}

int RemoteDesktopViewPanel::sessionCount() const {
    return order_.size();
}

QVector<QString> RemoteDesktopViewPanel::sessionPeerIds() const {
    return order_;
}
