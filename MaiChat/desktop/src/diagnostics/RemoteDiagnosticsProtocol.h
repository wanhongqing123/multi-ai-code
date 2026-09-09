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
