// 文件作用：桌面端跨设备 Session 的本机读取与操作入口；网络投递由外层负责。
#include "agent/DesktopSessionSyncHost.h"

#include <QDir>
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
    } else if (std::holds_alternative<MaiReasoningPart>(part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("reasoning"));
    } else if (const auto* body = std::get_if<MaiToolPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("tool"));
        value.insert(QStringLiteral("tool"), text(body->tool));
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

void DesktopSessionSyncHost::setDefaultWorkspaceDirectory(const QString& directory) {
    defaultWorkspaceDirectory_ = directory;
}

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
    QJsonArray permissions;
    for (const MaiPermissionRequest& request : controller_.agent().listPendingPermissions()) {
        if (request.sessionId != session.id) continue;
        permissions.append(QJsonObject{
            {QStringLiteral("id"), text(request.id)},
            {QStringLiteral("tool"), text(request.toolName)},
            {QStringLiteral("input"), text(request.arguments)},
            {QStringLiteral("allowForSession"), request.allowForSession},
            {QStringLiteral("rememberOnApproval"), request.rememberOnApproval},
            {QStringLiteral("fileCount"), int(request.approvalKeys.size())}});
    }
    QJsonArray questions;
    for (const MaiQuestionRequest& request : controller_.agent().listPendingQuestions()) {
        if (request.sessionId != session.id) continue;
        QJsonArray options;
        for (const std::string& option : request.options) options.append(text(option));
        questions.append(QJsonObject{{QStringLiteral("id"), text(request.id)},
                                     {QStringLiteral("question"), text(request.question)},
                                     {QStringLiteral("options"), options}});
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("messages"), messages},
            {QStringLiteral("permissions"), permissions},
            {QStringLiteral("questions"), questions}};
}

QJsonObject DesktopSessionSyncHost::applyCommand(const QString& sessionId, const QJsonObject& payload) {
    const QString action = payload.value(QStringLiteral("action")).toString();
    if (sessionId == QStringLiteral("session_lifecycle")) {
        if (action == QStringLiteral("session_create")) {
            QString path = payload.value(QStringLiteral("workspace_path")).toString();
            if (path.isEmpty()) {
                path = defaultWorkspaceDirectory_;
                if (path.isEmpty() || QDir(path).isRoot() || !QDir().mkpath(path))
                    return failure(QStringLiteral("默认工作目录不可用"));
            } else if (path.size() > 4096 || !QFileInfo(path).isAbsolute()) {
                return failure(QStringLiteral("工作目录必须是已存在的绝对路径"));
            }
            const QFileInfo directory(path);
            const QString canonical = directory.canonicalFilePath();
            if (!directory.isDir() || canonical.isEmpty())
                return failure(QStringLiteral("工作目录不存在或不可访问"));
            const QString id = controller_.createSession(canonical);
            if (id.isEmpty()) return failure(controller_.lastError());
            return {{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), id}};
        }
        if (action == QStringLiteral("session_delete")) {
            const QString target = payload.value(QStringLiteral("target_session_id")).toString();
            MaiSession targetSession;
            if (target.isEmpty() || !controller_.agent().getSession(target.toStdString(), targetSession) ||
                !targetSession.isRoot())
                return failure(QStringLiteral("会话不存在"));
            const auto deleted = controller_.agent().submit(MaiDeleteSession{target.toStdString()});
            if (!deleted) return failure(text(deleted.error().message()));
            return {{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), target}};
        }
        if (action == QStringLiteral("session_set_workspace")) {
            const QString target = payload.value(QStringLiteral("target_session_id")).toString();
            const QString path = payload.value(QStringLiteral("workspace_path")).toString();
            MaiSession targetSession;
            if (target.isEmpty() || !controller_.agent().getSession(target.toStdString(), targetSession) ||
                !targetSession.isRoot())
                return failure(QStringLiteral("会话不存在"));
            const QFileInfo directory(path);
            const QString canonical = directory.canonicalFilePath();
            if (path.isEmpty() || path.toUtf8().size() > 4096 || !directory.isAbsolute() ||
                !directory.isDir() || canonical.isEmpty())
                return failure(QStringLiteral("工作目录必须是已存在的绝对路径"));
            if (!controller_.setSessionDirectory(target, canonical))
                return failure(controller_.lastError());
            return {{QStringLiteral("ok"), true},
                    {QStringLiteral("session_id"), target},
                    {QStringLiteral("workspace_path"), canonical}};
        }
        return failure(QStringLiteral("不支持的会话操作"));
    }
    MaiSession session;
    if (!controller_.agent().getSession(sessionId.toStdString(), session) || !session.isRoot())
        return failure(QStringLiteral("会话不存在"));
    if (action == QStringLiteral("prompt")) {
        const QString message = payload.value(QStringLiteral("text")).toString();
        if (message.isEmpty() || message.toUtf8().size() > 16 * 1024) return failure(QStringLiteral("消息为空或过长"));
        if (!controller_.sendPrompt(sessionId, message)) return failure(controller_.lastError());
        const QJsonArray recent = historyPage(sessionId, {}, 2).value(QStringLiteral("messages")).toArray();
        for (const QJsonValue& value : recent) {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("role")).toString() == QStringLiteral("user"))
                return {{QStringLiteral("ok"), true},
                        {QStringLiteral("user_message_id"), candidate.value(QStringLiteral("id"))}};
        }
    } else if (action == QStringLiteral("stop")) {
        if (!controller_.interrupt(sessionId)) return failure(controller_.lastError());
    } else if (action == QStringLiteral("permission")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        const QString decision = payload.value(QStringLiteral("decision")).toString();
        if (id.isEmpty() || id.size() > 128) return failure(QStringLiteral("权限请求无效"));
        if (decision != QStringLiteral("denied") && decision != QStringLiteral("approved") &&
            decision != QStringLiteral("approved_for_session"))
            return failure(QStringLiteral("权限决定无效"));
        const bool ok = decision == QStringLiteral("denied") ? controller_.denyPermission(id)
                        : decision == QStringLiteral("approved") ? controller_.approvePermission(id)
                        : decision == QStringLiteral("approved_for_session")
                            ? controller_.approvePermission(id, true) : false;
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
