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

    // 30 条/秒是全客户端共享的总配额，留出余量按 24 用。
    static constexpr int kBudgetPerSecond = 24;
    // 桶里至少还剩这么多令牌才允许发移动，给按键留爆发余量。
    static constexpr double kMoveMinTokens = 8.0;
    // 移动最快 20Hz，剩下的额度留给按键。
    static constexpr qint64 kMoveIntervalMs = 50;

private:
    void resetQueues();

    bool sessionActive_ = false;
    QString sessionId_;
    QRectF contentRect_;
    quint32 unreliableSequence_ = 0;
    quint32 reliableSequence_ = 0;

    bool hasPendingMove_ = false;
    double pendingMoveX_ = 0.0;
    double pendingMoveY_ = 0.0;
    QVector<Event> reliableQueue_;

    double tokens_ = static_cast<double>(kBudgetPerSecond);
    qint64 lastRefillMs_ = 0;
    qint64 lastMoveSentMs_ = 0;
};

}  // namespace RemoteInput
