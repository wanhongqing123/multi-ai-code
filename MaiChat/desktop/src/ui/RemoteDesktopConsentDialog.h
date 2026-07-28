#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

// 被控端「有人值守」模式下的确认弹窗。
//
// 倒计时结束自动拒绝——远程桌面是高危授权，无人应答时必须默认收紧，
// 而不是一直挂着等人误点。
class RemoteDesktopConsentDialog final : public QDialog {
    Q_OBJECT

public:
    RemoteDesktopConsentDialog(const QString& fromUserId,
                               int timeoutMs,
                               QWidget* parent = nullptr);

    // 剩余秒数，供测试断言倒计时行为。
    int remainingSeconds() const;

    // 供测试推进倒计时，避免真的等 60 秒。
    void tickForTest();

private:
    void buildUi(const QString& fromUserId);
    void applyStyle();
    void updateCountdownLabel();
    void handleTick();

    QLabel* countdownLabel_ = nullptr;
    QPushButton* allowButton_ = nullptr;
    QPushButton* rejectButton_ = nullptr;
    QTimer* countdownTimer_ = nullptr;
    int remainingSeconds_ = 0;
};
