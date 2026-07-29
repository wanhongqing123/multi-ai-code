#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>
#include <QWidget>

// 一路远程桌面会话的卡片：顶部信息条 + 画面 + 状态条。
//
// 画面由 TRTC 直接渲染到 renderWindowHandle() 的原生句柄上，本类不逐帧绘制。
// 做成卡片而非整页，是为了多路并发时能直接平铺，无需再改容器。
class RemoteDesktopSessionCard final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteDesktopSessionCard(QString peerUserId, QWidget* parent = nullptr);

    const QString& peerUserId() const { return peerUserId_; }

    void setStreamActive(bool active);
    void setStatusText(const QString& text);

    void* renderWindowHandle() const;
    QString statusText() const;
    bool isStreamActive() const;

private:
    void buildUi();
    void applyStyle();
    void refreshDuration();

    QString peerUserId_;
    QWidget* renderSurface_ = nullptr;
    QLabel* placeholderLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTimer* durationTimer_ = nullptr;
    QElapsedTimer elapsed_;
    bool streamActive_ = false;
};
