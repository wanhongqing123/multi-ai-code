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

    // 文档写 30 条/秒，SDK 源码实际按 40 放行（trtc_message_sender.cc:25）。
    // 我们按 28 走，把差值留作安全垫，理由是那边的超限惩罚很重：被拒的消息
    // 照样计数且不回退，所以只要瞬时抖动碰一次上限，整个 1 秒窗口就全废。
    // 贴着 40 跑省下来的那点延迟，不值当去换这个风险。
    static constexpr int kBudgetPerSecond = 28;
    // 任意 1 秒窗口里给按键留出的名额，移动不许占用。
    static constexpr int kMoveReserveForKeys = 6;
    // 移动发包间隔 40ms。实际速率还受配额约束（28 - 6 = 22 条/秒），
    // 所以稳态大约 22Hz。
    //
    // 注意这里限的是**包数**不是事件数：一个包带整段轨迹，22Hz 的发包频率
    // 照样能还原上百 Hz 的移动采样。所以提高发包频率只降延迟、不增保真度——
    // 保真度已经由轨迹合批解决了。
    static constexpr qint64 kMoveIntervalMs = 40;
    static constexpr qint64 kWindowMs = 1000;
    // 单包最多带几个轨迹点。上限由 16KB/秒的字节配额倒推：
    // 每点约 32 字节，12 点约 400 字节，25 包/秒约 10KB/秒，留足余量。
    static constexpr int kMaxMovePointsPerPacket = 12;

private:
    void resetQueues();
    // 滑动窗口：丢掉 1 秒前的记录后，返回窗口内已发条数 / 字节数。
    void pruneHistory(qint64 nowMs);
    int windowCount(qint64 nowMs);
    int windowBytes(qint64 nowMs);
    // 条数与字节数两个配额都得留得下这个包。
    bool canSend(qint64 nowMs, int payloadBytes, int countLimit);
    void recordSend(qint64 nowMs, int payloadBytes);

    bool sessionActive_ = false;
    QString sessionId_;
    QRectF contentRect_;
    quint32 unreliableSequence_ = 0;
    quint32 reliableSequence_ = 0;

    // 待发的移动轨迹（而不是只留最后一点）：拖拽画线时中间点就是内容本身，
    // 丢掉会把曲线拉成直线。整段塞进一个包，不额外占配额。
    QVector<QPointF> pendingMovePath_;
    QVector<Event> reliableQueue_;

    // 最近 1 秒内每次发送的时刻与字节数。用滑动窗口而不是令牌桶：令牌桶的
    // 容量本身就是突发额度，满桶起步时"桶容量 + 一秒补充量"会超出上限。
    QVector<QPair<qint64, int>> sendHistory_;
    qint64 lastMoveSentMs_ = 0;
};

}  // namespace RemoteInput
