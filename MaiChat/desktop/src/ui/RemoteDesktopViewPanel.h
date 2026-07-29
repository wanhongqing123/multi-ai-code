#pragma once

#include <QHash>
#include <QLabel>
#include <QStackedWidget>
#include <QVector>
#include <QWidget>

class QGridLayout;
class RemoteDesktopSessionCard;

// 远程桌面预览页：画面在应用内成页展示，不弹独立窗口。
//
// 每一路会话是一张 RemoteDesktopSessionCard，按 peerUserId 索引。只有一路时
// 卡片铺满整页（观感等同单会话全屏），多路时自动平铺 —— 将来接多路只需照常
// beginSession()，这里不必再改。
class RemoteDesktopViewPanel final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteDesktopViewPanel(QWidget* parent = nullptr);

    // 无会话时显示空态；发起后切到画面区。
    void showIdle();
    void beginSession(const QString& peerUserId);
    void endSession(const QString& peerUserId);

    void setStreamActive(const QString& peerUserId, bool active);
    void setStatusText(const QString& peerUserId, const QString& text);
    // 交给 TRTC 的渲染目标；未知 peer 返回 nullptr。
    void* renderWindowHandle(const QString& peerUserId) const;

    // 单路便捷入口：作用于当前唯一一路会话，没有会话时是空操作。
    void showConnecting(const QString& peerUserId) { beginSession(peerUserId); }
    void setStreamActive(bool active);
    void setStatusText(const QString& text);
    void* renderWindowHandle() const;
    QString statusText() const;
    bool isStreamActive() const;

    bool isSessionVisible() const;
    int sessionCount() const;
    QVector<QString> sessionPeerIds() const;
    RemoteDesktopSessionCard* cardFor(const QString& peerUserId) const;

    // 全屏：只留目标卡片并铺满，其余卡片藏起来（不销毁，画面不断流）。
    void enterFullScreen(const QString& peerUserId);
    void exitFullScreen();
    void toggleFullScreen(const QString& peerUserId);
    bool isFullScreen() const;
    QString fullScreenPeerId() const;

signals:
    // 交给 MainWindow 收起侧栏、切窗口全屏；面板自己不碰窗口状态。
    void fullScreenChanged(bool fullScreen);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildUi();
    void applyStyle();
    // 卡片数变化后重排网格：1 路占满，多路平铺。
    void relayoutCards();
    RemoteDesktopSessionCard* soleCard() const;

    QStackedWidget* stack_ = nullptr;
    QWidget* idlePage_ = nullptr;
    QWidget* gridPage_ = nullptr;
    QGridLayout* gridLayout_ = nullptr;
    QHash<QString, RemoteDesktopSessionCard*> cards_;
    // 保留插入顺序，避免 QHash 遍历导致卡片位置随机跳动。
    QVector<QString> order_;
    // 非空表示正处于全屏，值是被放大的那一路。
    QString fullScreenPeerId_;
};
