#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// 远程输入协议：控制端 → 被控端的鼠标/键盘事件编解码。
//
// 走 TRTC sendCustomCmdMsg，按可靠性分两条通道（cmdID 见 kCmdIdUnreliable /
// kCmdIdReliable）。本文件只做纯粹的编解码，不碰传输、不碰注入，便于单测。
//
// 坐标一律归一化到 [0,1]（相对被控端被采集的那块屏幕），不传像素：
// 两端分辨率、DPI 缩放、控制端窗口大小都可以不一致。
namespace RemoteInput {

constexpr int kProtocolVersion = 1;

// 移动走不可靠不有序（丢了下一包就纠正回来）；按键/滚轮/文本走可靠有序
// （丢一条会留下"按下没抬起"的悬空状态）。同一 cmdID 内 reliable/ordered
// 必须前后一致，这是 TRTC 的硬约束（ITRTCCloud.h:1381），所以只能拆成两个 ID。
constexpr int kCmdIdUnreliable = 2;
constexpr int kCmdIdReliable = 3;

// TRTC 单包上限 1KB（ITRTCCloud.h:1379）。超出的包会被中间路由丢弃，
// 编码方必须自己拆分。
constexpr int kMaxPacketBytes = 1024;

// 30 条/秒是**整个客户端**的总配额，两个 cmdID 共享，还与 sendSEIMsg 共享
// （ITRTCCloud.h:1378）——不是每个 cmdID 各 30 条。移动事件若按 30Hz 发就会
// 吃光全部预算，按键一条都挤不出去。发送端必须用统一预算并让按键优先。
constexpr int kMaxMessagesPerSecond = 30;
// 另有 16KB/秒的总字节配额（ITRTCCloud.h:1380）。
constexpr int kMaxBytesPerSecond = 16 * 1024;

// 包从哪条通道走。两条通道的丢包语义完全不同（不可靠通道丢包是设计的一部分，
// 可靠通道丢包说明出事了），收发两端的处理策略都要据此分开，所以放协议层。
enum class Channel {
    Unreliable,  // 移动：丢了下一包就纠正回来
    Reliable     // 按键/滚轮/文本：丢了会留下悬空状态
};

enum class EventType {
    Unknown,      // 解析不出来的事件：跳过，不让整包报废
    MouseMove,
    MouseButton,
    MouseWheel,
    Key,
    Text,
    ReleaseAll    // 全部抬起：会话收尾与看门狗兜底用
};

enum class MouseButton { Left, Right, Middle };

struct Event {
    EventType type = EventType::Unknown;
    // 归一化坐标，鼠标类事件有效。
    double x = 0.0;
    double y = 0.0;
    MouseButton button = MouseButton::Left;
    // 按下为 true，抬起为 false。MouseButton / Key 有效。
    bool pressed = false;
    // 滚轮增量，一格 120，正数向上。
    int wheelDelta = 0;
    // 平台虚拟键码（Windows VK_*）。
    quint32 keyCode = 0;
    // 已上屏的文本（IME 成词结果、手机软键盘输入）。
    QString text;
};

struct Packet {
    int protocolVersion = 0;
    // 绑定会话：被控端据此拒收上一场会话的残留输入。
    QString sessionId;
    // 自增序号。被控端发现跳号跨度过大时先全抬再继续，避免悬空按键。
    quint32 sequence = 0;
    QVector<Event> events;
};

QByteArray encodePacket(const Packet& packet);

// 任何无法可靠解析的输入都返回 false 并保持 out 不变。这个函数直接吃网络
// 来的字节，必须对畸形输入免疫：坏包一律丢弃，绝不半解析后驱动注入。
bool decodePacket(const QByteArray& payload, Packet* out);

// 编出来的包是否塞得进单个 TRTC 包。发送方据此决定是否拆分。
bool fitsInOnePacket(const Packet& packet);

// 坐标钳到 [0,1]。编解码两侧都会调用：坏包不该把光标甩到屏幕外。
double clampNormalized(double value);

}  // namespace RemoteInput
