// 文件作用：桌面端共用界面；SharingIndicatorBar 负责共享状态提示和用户操作反馈，本文件给出其声明。
// 类型关系：SharingIndicatorBar final : public QWidget；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 SharingIndicatorBar 的类型和函数声明；参数、返回值和对象生命周期以此为准。
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
