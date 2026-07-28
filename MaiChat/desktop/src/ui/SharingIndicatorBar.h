#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

// 被控端共享期间的常驻指示条。
//
// 设计上刻意不提供隐藏入口：无人值守模式下用户可能不在电脑前，回来后必须
// 一眼能看出"刚才屏幕被看过、现在还在被看"。
class SharingIndicatorBar final : public QWidget {
    Q_OBJECT

public:
    explicit SharingIndicatorBar(QWidget* parent = nullptr);

    // 开始共享；bar 变可见并从 00:00 开始计时。
    void startSharing(const QString& peerUserId);
    void stopSharing();

    QString currentText() const;

signals:
    void stopRequested();

private:
    void applyStyle();
    void refreshText();

    QLabel* textLabel_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QTimer* tickTimer_ = nullptr;
    QElapsedTimer elapsed_;
    QString peerUserId_;
};
