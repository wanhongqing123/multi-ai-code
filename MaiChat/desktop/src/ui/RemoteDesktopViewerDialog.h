#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

// 控制端观看窗：显示远端屏幕画面。
//
// 画面由 TRTC 直接渲染到 renderSurface() 的原生窗口句柄上，本类不参与
// 逐帧绘制——所以这里只负责窗口骨架、状态提示与断开入口。
class RemoteDesktopViewerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit RemoteDesktopViewerDialog(const QString& peerUserId, QWidget* parent = nullptr);

    // 交给 TRTC 的渲染目标。必须在窗口显示后取，否则句柄尚未创建。
    void* renderWindowHandle() const;

    // 画面到达前显示"正在连接"，到达后隐藏占位提示。
    void setStreamActive(bool active);
    void setStatusText(const QString& text);

    QString statusText() const;
    bool isStreamActive() const;

signals:
    void disconnectRequested();

private:
    void buildUi(const QString& peerUserId);
    void applyStyle();
    void refreshDuration();

    QWidget* renderSurface_ = nullptr;
    QLabel* placeholderLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QTimer* durationTimer_ = nullptr;
    QElapsedTimer elapsed_;
    bool streamActive_ = false;
};
