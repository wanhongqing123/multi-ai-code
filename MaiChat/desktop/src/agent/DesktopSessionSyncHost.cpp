// 文件作用：桌面端跨设备 Session 的本机读取与操作入口；网络投递由外层负责。
#include "agent/DesktopSessionSyncHost.h"

#include <QFileInfo>

#include <variant>

#include "agent/AgentController.h"

namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), int(value.size()));
}

QJsonObject failure(const QString& message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

QJsonObject serializePart(const MaiMessagePart& part) {
    QJsonObject value{{QStringLiteral("id"), text(part.id)}};
    if (const auto* body = std::get_if<MaiTextPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("text"));
        value.insert(QStringLiteral("text"), text(body->text));
    } else if (const auto* body = std::get_if<MaiReasoningPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("reasoning"));
        value.insert(QStringLiteral("text"), text(body->text));
    } else if (const auto* body = std::get_if<MaiToolPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("tool"));
        value.insert(QStringLiteral("tool"), text(body->tool));
        value.insert(QStringLiteral("input"), text(body->input));
        value.insert(QStringLiteral("output"), text(body->output));
        value.insert(QStringLiteral("error"), text(body->error));
        value.insert(QStringLiteral("state"), QString::fromLatin1(maiToolStateToString(body->state)));
    } else if (const auto* body = std::get_if<MaiImagePart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("image"));
        value.insert(QStringLiteral("mimeType"), text(body->mimeType));
        value.insert(QStringLiteral("filename"), QFileInfo(text(body->path)).fileName());
        value.insert(QStringLiteral("remotePartId"), text(part.id));
    } else if (const auto* body = std::get_if<MaiVideoPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("video"));
        value.insert(QStringLiteral("mimeType"), text(body->mimeType));
        value.insert(QStringLiteral("filename"), QFileInfo(text(body->path)).fileName());
        value.insert(QStringLiteral("remotePartId"), text(part.id));
    } else if (const auto* body = std::get_if<MaiQuotePart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("quote"));
        value.insert(QStringLiteral("messageId"), text(body->messageId));
        value.insert(QStringLiteral("preview"), text(body->preview));
    }
    return value;
}

QJsonObject serializeMessage(const MaiMessage& message, bool busy) {
    QJsonArray parts;
    for (const auto& part : message.parts) parts.append(serializePart(part));
    return {{QStringLiteral("id"), text(message.id)},
            {QStringLiteral("role"), QString::fromLatin1(maiRoleToString(message.role))},
            {QStringLiteral("created"), double(message.created)},
            {QStringLiteral("completed"), double(message.completed)},
            {QStringLiteral("active"), busy && message.isInProgress()},
            {QStringLiteral("parts"), parts}};
}

}  // namespace

DesktopSessionSyncHost::DesktopSessionSyncHost(AgentController& controller) : controller_(controller) {}

QJsonArray DesktopSessionSyncHost::rootSessions() const {
    QJsonArray result;
    for (const MaiSession& session : controller_.agent().listSessions()) {
        if (!session.isRoot()) continue;
        result.append(QJsonObject{{QStringLiteral("id"), text(session.id)},
                                  {QStringLiteral("title"), text(session.title)},
                                  {QStringLiteral("updated_at"), double(session.updated)},
                                  {QStringLiteral("busy"), controller_.agent().isBusy(session.id)}});
    }
    return result;
}

QJsonObject DesktopSessionSyncHost::historyPage(const QString& sessionId, const QString& beforeId, int limit) const {
    MaiSession session;
    if (limit < 1 || limit > 20 || !controller_.agent().getSession(sessionId.toStdString(), session) ||
        !session.isRoot())
        return failure(QStringLiteral("会话不存在或页大小无效"));
    QJsonArray messages;
    const auto page =
        controller_.agent().listMessagesPage(sessionId.toStdString(), beforeId.toStdString(), size_t(limit));
    for (const MaiMessage& message : page)
        messages.append(serializeMessage(message, controller_.agent().isBusy(session.id)));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), messages}};
}

QJsonObject DesktopSessionSyncHost::applyCommand(const QString& sessionId, const QJsonObject& payload) {
    MaiSession session;
    if (!controller_.agent().getSession(sessionId.toStdString(), session) || !session.isRoot())
        return failure(QStringLiteral("会话不存在"));
    const QString action = payload.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("prompt")) {
        const QString message = payload.value(QStringLiteral("text")).toString();
        if (message.isEmpty() || message.toUtf8().size() > 16 * 1024) return failure(QStringLiteral("消息为空或过长"));
        if (!controller_.sendPrompt(sessionId, message)) return failure(controller_.lastError());
    } else if (action == QStringLiteral("stop")) {
        if (!controller_.interrupt(sessionId)) return failure(controller_.lastError());
    } else if (action == QStringLiteral("permission")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        const QString decision = payload.value(QStringLiteral("decision")).toString();
        if (id.isEmpty() || id.size() > 128) return failure(QStringLiteral("权限请求无效"));
        if (decision != QStringLiteral("deny") && decision != QStringLiteral("approve_once") &&
            decision != QStringLiteral("approve_session"))
            return failure(QStringLiteral("权限决定无效"));
        const bool ok = decision == QStringLiteral("deny")              ? controller_.denyPermission(id)
                        : decision == QStringLiteral("approve_once")    ? controller_.approvePermission(id)
                        : decision == QStringLiteral("approve_session") ? controller_.approvePermission(id, true)
                                                                        : false;
        if (!ok) return failure(controller_.lastError());
    } else if (action == QStringLiteral("answer")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        const QString answer = payload.value(QStringLiteral("text")).toString();
        if (id.isEmpty() || id.size() > 128 || answer.isEmpty() || answer.size() > 16 * 1024)
            return failure(QStringLiteral("回答请求无效"));
        if (!controller_.answerQuestion(id, answer)) return failure(controller_.lastError());
    } else if (action == QStringLiteral("rename")) {
        const QString title = payload.value(QStringLiteral("title")).toString();
        if (title.isEmpty() || title.size() > 120) return failure(QStringLiteral("会话标题无效"));
        const auto result =
            controller_.agent().submit(MaiUpdateSession{sessionId.toStdString(), title.toStdString(), "", ""});
        if (!result) return failure(text(result.error().message()));
    } else {
        return failure(QStringLiteral("不支持的远端操作"));
    }
    return {{QStringLiteral("ok"), true}};
}
