#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// 远程排障协议（A 端视角）。
//
// A = 发起排障的人，B = 出故障的那台（当前聊天对象），C = 收报告的好友。
// A 向 B 发一条**可见的普通文本** `/diagnostics <requestId>`；B 的宿主
// 独立于 AI 识别它并回一个 JSON 附件。这里只放纯逻辑：生成/校验 requestId、
// 命令与附件名的构造与解析、报告 JSON 的白名单裁剪。
//
// 这个模块直接吃对端可控的数据（文件名、JSON），必须对畸形输入免疫：
// 任何解析失败都返回"不接受"，不抛异常、不部分采信。
namespace RemoteDiagnostics {

// 报告体积上限。超过一律拒收——排障报告不该有这个量级，
// 真到了这个量级也说明对端在塞正文。
constexpr qint64 kMaxReportBytes = 2 * 1024 * 1024;

// 只认这一个 schema 版本。将来加版本要显式放行，不做"大于等于"的宽松判断。
constexpr int kSchemaVersion = 1;

// B 迟迟不回时的等待上限。超时不是失败——要出一份注明缺失的部分报告。
constexpr int kResponseTimeoutMs = 60 * 1000;

// 生成一个新的 requestId（小写 UUID，无花括号）。
QString newRequestId();

// 是否是合法的 requestId。大写、带花括号、长度不对一律不接受：
// 两端要按完全相同的字符串比对，放宽一点就会出现"看起来一样但不相等"。
bool isValidRequestId(const QString& requestId);

// 发给 B 的那条人类可见文本。
QString formatCommand(const QString& requestId);

// 从任意文本里解出 requestId；不是这条命令就返回空。
QString parseCommand(const QString& text);

// B 回传附件应有的文件名。
QString attachmentFileName(const QString& requestId);

// 文件名是否属于这个 requestId。文件名和 JSON 里的 requestId 都要查，
// 只查一个就能被"文件名对、内容是别人的"绕过。
bool matchesAttachmentFileName(const QString& fileName, const QString& requestId);

// 白名单裁剪的结果。
struct ParsedReport {
    bool accepted = false;
    // 不被接受的原因，直接写进报告给人看，不吞掉。
    QString rejectionReason;
    QJsonObject report;
    // 被丢弃的顶层/嵌套字段名，供报告里说明"对端给了但我们没收"。
    QStringList droppedFields;
};

// B 明确回绝时的固定原因。
//
// 只认**已知的固定提示**并映射成这些枚举；**绝不把远端文本复制进报告**——
// 那是对端可控的内容，抄进去等于把它转发给 C。
enum class FailureReason {
    None,                 // 不是回绝（普通聊天文本）
    Unsupported,          // 旧版 B：不认识这条命令
    RateLimited,          // B 限流
    CollectionFailed,     // B 采集失败
    ServiceUnavailable    // B 的采集服务不可用
};

struct FailureReceipt {
    bool recognized = false;
    FailureReason reason = FailureReason::None;
};

// 从 B 的一条文本里识别「本次请求被回绝」。
//
// 必须同时满足：文本携带**本次**的 requestId，且首行是**已知固定提示**之一。
// 识别到就可以提前结束等待——明知被拒还空等满 60 秒是纯粹浪费用户时间。
// 认不出来的一律返回 recognized=false（当普通聊天文本，继续等），
// 不做模糊匹配：猜错会把用户的正常发言当成回绝。
FailureReceipt parseFailureReceipt(const QString& text, const QString& expectedRequestId);

// 这份报告是给谁看的：把固定原因转成给人读的说明。
QString describeFailureReason(FailureReason reason);

// 按白名单裁剪 B 的报告。
//
// `expectedRequestId` 与 `expectedSenderId` 必须与附件实际来源一致，
// 否则整份拒收：伪造的报告不能因为"格式正确"就被并进去。
// 未知字段一律丢弃并记名，绝不原样透传——透传等于把对端可控的内容
// 塞进发给 C 的报告里。
ParsedReport parseReport(const QByteArray& payload,
                         const QString& fileName,
                         const QString& expectedRequestId,
                         const QString& senderId,
                         const QString& expectedSenderId);

}  // namespace RemoteDiagnostics
