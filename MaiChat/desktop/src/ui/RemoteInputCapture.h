#pragma once

#include <QObject>
#include <QPointer>
#include <QWidget>

class QTimer;

#include "remote/RemoteInputSender.h"

// 控制端采集：把画面控件上的鼠标键盘事件翻译成归一化输入，喂给 sender。
//
// 独立成一个类是有意的：画面区是 TRTC 渲染的原生窗口，SDK 可能在上面盖了
// 自己的子窗口把鼠标消息吃掉。真遇上了只需换掉这个类的采集方式（透明覆盖层
// 或子类化 SDK 子窗口），控制器、协议、注入器一律不动。
//
// 只有 setEnabled(true) 期间才采集；关掉时立刻发一次"全部抬起"，
// 避免本地松手时对端还按着。
class RemoteInputCapture : public QObject {
    Q_OBJECT

public:
    RemoteInputCapture(RemoteInput::RemoteInputSender& sender, QObject* parent = nullptr);

    // 绑定画面控件。传 nullptr 解绑。可重复调用。
    void attachTo(QWidget* surface);
    // 远端画面的原始分辨率，用于算黑边。拿不到时不采集坐标。
    void setRemoteVideoSize(const QSize& size);

    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

signals:
    // 急停热键被按下（Ctrl+Alt+Shift+Q）。上层应立刻关闭控制。
    void releaseControlRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshContentRect();
    // 返回 false 表示这个位置不该产生输入（落在黑边上且不是拖拽中）。
    bool mapPosition(const QPointF& pos, double* x, double* y) const;
    // 控制期间必须开画面控件的 mouseTracking，否则 Qt 只在按住键时才投递
    // MouseMove，悬空移动完全采集不到。退出控制时还原原值。
    void beginMouseTracking();
    void endMouseTracking();
    // 每秒往应用日志写一条采集汇总。只在控制期间跑。
    void flushTrace();
    // 把控件尺寸、远端画面尺寸、算出来的内容区一起打出来。坐标偏移只可能来自
    // 这三个数的关系，原样记下就能手算复核，不必再让人重试一轮去猜。
    void logGeometry(const QString& reason) const;

    RemoteInput::RemoteInputSender& sender_;
    QPointer<QWidget> surface_;
    QSize remoteVideoSize_;
    bool enabled_ = false;
    bool mouseTrackingApplied_ = false;
    bool mouseTrackingRestore_ = false;
    // 我们**确实发出过按下**的键。只有它们的抬起才该转发：在黑边上点一下
    // 不会产生按下，也就不该凭空发一个抬起过去。
    // 用集合而不是单个 bool，是为了左键拖拽途中右键点一下也能各自配对。
    Qt::MouseButtons pressedButtons_ = Qt::NoButton;

    // 诊断计数：区分"Qt 没收到事件"与"收到了但没入队"。前者说明事件在到达
    // Qt 之前就被 SDK 的渲染子窗口截走了，后者说明坐标映射有问题。
    QTimer* traceTimer_ = nullptr;
    int traceMoveSeen_ = 0;
    int traceMoveQueued_ = 0;
    int traceButtonSeen_ = 0;
    int traceWheelSeen_ = 0;
    int traceKeySeen_ = 0;
    // 每秒留一个坐标样本：光有计数说明不了映射对不对，得能拿原始像素点和
    // 归一化结果去手算。被判成黑边丢掉的那个点单独留，用来判断是真黑边还是
    // 内容区算小了。
    QPointF traceLastLocal_;
    QPointF traceLastDroppedLocal_;
    double traceLastNormX_ = 0.0;
    double traceLastNormY_ = 0.0;
};
