#pragma once

#include <QString>

#include "model/RemoteIMMessage.h"

// 引用回复：把被引用的消息压成一份**发送时的快照**。
//
// 为什么是快照而不是只存 ID：被引用的消息可能根本不在对端本地——对端刚装、
// 本地已清理、或者它落在还没加载的分页里。只存 ID 的话引用块会渲染成空白，
// 而空白引用块在会话里非常显眼。
namespace MessageQuote {

// 摘要的显示字符上限，超出截断并补省略号。
inline constexpr int kDigestLimit = 120;

// 被引用消息的内容类别：text / image / file / video / voice。
QString kindOf(const RemoteIMMessage& message);

// 摘要快照。带附件的消息优先用真实配文，没有配文才退到类型占位。
// 文件消息在没有配文时显示「[文件] 文件名」——引用块在会话内部，
// 文件名对辨认「你回复的是哪一份」有实际价值，隐私边界与原文件消息相同。
// 但只取 basename，绝不放本地路径。
QString digestOf(const RemoteIMMessage& message);

// 从本地主键还原出**原始 SDK 消息 ID**。
//
// desktop 把 SDK id 直接当主键用，格式是 `<sdkMsgId>#<elem下标>`：一条 SDK 消息
// 可含多个 elem，会被拆成多条 RemoteIMMessage，需要各自唯一。而 iOS/Android 存的是
// 不带下标的原始 msgID，所以协议里只能放原始 ID，各端自己映射回本地。
//
// 拿不到 SDK id 时主键会退化成随机 UUID。那种 id 跨端无法解析，必须返回空串，
// 绝不能冒充可解析 ID 发出去。判据是「末尾是 #<十进制数字>」——只剥末尾，
// 不按第一个 # 截断，因为协议层不该偷偷假设 SDK id 自身不含 #。
QString sdkMessageIdOf(const QString& localMessageId);

// 组装引用块。msgId 拿不到时留空：引用块照常显示，只是不提供「跳到原文」。
RemoteIMQuote quoteFor(const RemoteIMMessage& message);

}  // namespace MessageQuote
