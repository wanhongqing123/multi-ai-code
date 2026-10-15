// 文件作用：桌面端共用界面；RemoteDesktopConsentDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：RemoteDesktopConsentDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 RemoteDesktopConsentDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
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
