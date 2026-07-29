#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

// 远程桌面预览页：画面在应用内成页展示，不弹独立窗口。
//
// 画面由 TRTC 直接渲染到 renderWindowHandle() 的原生句柄上，本类不参与
// 逐帧绘制，只负责页面骨架、空态与状态提示。
class RemoteDesktopViewPanel final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteDesktopViewPanel(QWidget* parent = nullptr);

    // 无会话时显示空态；发起后切到画面区。
    void showIdle();
    void showConnecting(const QString& peerUserId);
    void setStreamActive(bool active);
    void setStatusText(const QString& text);

    // 交给 TRTC 的渲染目标。
    void* renderWindowHandle() const;

    QString statusText() const;
    bool isStreamActive() const;
    bool isSessionVisible() const;

private:
    void buildUi();
    void applyStyle();
    void refreshDuration();

    QStackedWidget* stack_ = nullptr;
    QWidget* idlePage_ = nullptr;
    QWidget* sessionPage_ = nullptr;
    QWidget* renderSurface_ = nullptr;
    QLabel* placeholderLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTimer* durationTimer_ = nullptr;
    QElapsedTimer elapsed_;
    bool streamActive_ = false;
};
