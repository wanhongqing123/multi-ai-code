#pragma once

#include <QByteArray>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QVector>

#include "remote/RemoteInputProtocol.h"

// 控制端输入发送：坐标换算 + 合帧 + 配额调度。
//
// 不碰 Qt 控件也不碰 TRTC，只吃归一化坐标与事件、吐出待发的包，便于单测。
// 真正的 send 由调用方接到 ITrtcEngine 上。
namespace RemoteInput {

// Fit 模式下画面在控件里实际占的矩形（等比缩放 + 居中，短边留黑边）。
// 坐标换算必须先减掉黑边，否则整体偏移。
QRectF fitContentRect(const QSizeF& widgetSize, const QSizeF& videoSize);

class RemoteInputSender {
public:
    struct OutgoingPacket {
        Channel channel = Channel::Reliable;
        QByteArray payload;
    };

    void beginSession(const QString& sessionId);
    void endSession();
    bool isSessionActive() const { return sessionActive_; }

    // 画面在控件内的实际矩形，由调用方按 fitContentRect 算好后设进来。
    void setContentRect(const QRectF& contentRect);
    const QRectF& contentRect() const { return contentRect_; }

    // 控件内坐标 → 归一化 [0,1]。落在黑边上返回 false：那里不对应远端屏幕的
    // 任何位置，硬映射会变成点到屏幕边缘，很意外。
    bool mapToNormalized(const QPointF& widgetPos, double* outX, double* outY) const;
    // 拖拽途中滑出画面区时用：钳到边界继续跟随，而不是原地不动。
    bool mapToNormalizedClamped(const QPointF& widgetPos, double* outX, double* outY) const;

    void queueMouseMove(double x, double y);
    void queueMouseButton(MouseButton button, bool pressed, double x, double y);
    void queueWheel(int delta, double x, double y);
    void queueKey(quint32 keyCode, bool pressed);
    void queueText(const QString& text);
    void queueReleaseAll();

    // 按配额吐出这一刻该发的包。调用方定时驱动（建议 20~25Hz）。
    QVector<OutgoingPacket> flush(qint64 nowMs);

    // 文档配额 30 条/秒（全客户端共享），按 28 用留 2 条余量。
    // SDK 源码实际按 40 放行，那 10 条不去吃，留作安全垫。
    static constexpr int kBudgetPerSecond = 28;
    // 任意 1 秒窗口里给按键留出的名额，移动不许占用。
    static constexpr int kMoveReserveForKeys = 6;
    // 移动最快 25Hz。再快对手感提升有限，却会挤掉按键的余量。
    static constexpr qint64 kMoveIntervalMs = 40;
    static constexpr qint64 kWindowMs = 1000;

private:
    void resetQueues();
    // 滑动窗口计数：丢掉 1 秒前的记录，返回窗口内已发条数。
    int windowCount(qint64 nowMs);
    void recordSend(qint64 nowMs);

    bool sessionActive_ = false;
    QString sessionId_;
    QRectF contentRect_;
    quint32 unreliableSequence_ = 0;
    quint32 reliableSequence_ = 0;

    bool hasPendingMove_ = false;
    double pendingMoveX_ = 0.0;
    double pendingMoveY_ = 0.0;
    QVector<Event> reliableQueue_;

    // 最近 1 秒内每次发送的时刻。用滑动窗口而不是令牌桶：令牌桶的容量本身
    // 就是突发额度，满桶起步时"桶容量 + 一秒补充量"会超出上限。
    QVector<qint64> sendTimestamps_;
    qint64 lastMoveSentMs_ = 0;
};

}  // namespace RemoteInput
