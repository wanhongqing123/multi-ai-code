#pragma once

#include <QString>

// 远程桌面信令：借道现有 IM 文本消息通道传输，不新增 IM 消息类型。
//
// 载荷形如：<不可见前缀>[remote-desktop]{"v":1,"type":"invite",...}
// 不可见前缀让信令不污染聊天记录展示；应用层识别后直接转交控制器，
// 既不入库也不产生未读红点。
namespace RemoteDesktopSignals {

enum class Type {
    Unknown,   // 不是信令（普通聊天文本），或版本/格式不被支持
    Invite,    // 控制端 → 被控端：请求查看屏幕
    Accept,    // 被控端 → 控制端：同意，附房间号
    Reject,    // 被控端 → 控制端：拒绝，附原因
    Stop,      // 任一端 → 对端：结束会话
    Notice     // 被控端 → 控制端：会话中的状态播报，附 noticeCode
};

// Notice 的取值。传码不传文案：文案属于展示层，控制端可以自己决定怎么呈现，
// 而且将来改文案不必要求两端同时升级。
namespace NoticeCodes {
// 被控端切到了安全桌面（UAC 提权框 / 锁屏 / Ctrl+Alt+Del）。
// 此时画面采不到、输入注不进，远程表现为"卡住 + 点不动"——必须告诉控制端
// 这是在等人操作，而不是断网或崩溃。
extern const char kSecureDesktopEntered[];
extern const char kSecureDesktopLeft[];
}  // namespace NoticeCodes

struct Signal {
    Type type = Type::Unknown;
    QString sessionId;
    QString roomId;
    // 无人值守模式下的密码证明；有人值守模式可空。明文密码永不出现在信令里。
    QString authProof;
    QString reason;
    // 仅 Notice 使用，取 NoticeCodes 里的常量。
    QString noticeCode;
    int protocolVersion = 0;
};

// 当前协议版本。解析时版本不匹配一律降级为 Unknown（宁可当普通消息，
// 也不要用错误的语义去驱动状态机）。
constexpr int kProtocolVersion = 1;

// 便于测试与排错：允许调用方拿到裸前缀。
QString signalPrefix();

// 这段文本是否是远程桌面信令（只看前缀，不做完整解析）。
bool isSignalText(const QString& text);

QString encodeSignal(const Signal& signal);

// 任何无法可靠解析的输入都返回 type=Unknown，绝不抛异常：
// 这个函数直接吃用户可控的 IM 文本，必须对畸形输入免疫。
Signal decodeSignal(const QString& text);

}  // namespace RemoteDesktopSignals
