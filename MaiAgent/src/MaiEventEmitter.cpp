#include "MaiEventEmitter.h"

#include "MaiIdGenerator.h"

MaiEventEmitter::MaiEventEmitter(MaiEventBus& bus) : bus_(bus) {}

namespace {

// 所有事件共有的字段只在这一处填。加新事件类型时不会漏掉 id 或 time——
// 漏了不会编译报错，只会在界面上表现成某些更新莫名其妙丢了。
MaiEvent makeEvent(MaiEventType type, const std::string& sessionId) {
    MaiEvent e;
    e.id = MaiIdGenerator::newEventId();
    e.type = type;
    e.sessionId = sessionId;
    e.time = MaiTime::getCurrentTime();
    return e;
}

}  // namespace

void MaiEventEmitter::emitSession(MaiEventType type, const std::string& sessionId,
                                  const std::string& detail) {
    MaiEvent e = makeEvent(type, sessionId);
    e.detail = detail;
    bus_.publish(e);
}

void MaiEventEmitter::emitMessage(MaiEventType type, const std::string& sessionId,
                                  const std::string& messageId) {
    MaiEvent e = makeEvent(type, sessionId);
    e.messageId = messageId;
    bus_.publish(e);
}

void MaiEventEmitter::emitPart(MaiEventType type, const std::string& sessionId,
                               const std::string& messageId, const std::string& partId) {
    MaiEvent e = makeEvent(type, sessionId);
    e.messageId = messageId;
    e.partId = partId;
    bus_.publish(e);
}

void MaiEventEmitter::emitDelta(const std::string& sessionId, const std::string& messageId,
                                const std::string& partId, const char* field,
                                std::string_view chunk) {
    MaiEvent e = makeEvent(MaiEventType::MessagePartDelta, sessionId);
    e.messageId = messageId;
    e.partId = partId;
    e.field = field;
    e.delta.assign(chunk);
    bus_.publish(e);
}

void MaiEventEmitter::emitPermissionAsked(const MaiPermissionRequest& request) {
    MaiEvent e = makeEvent(MaiEventType::PermissionAsked, request.sessionId);
    e.messageId = request.messageId;
    e.partId = request.partId;
    e.permissionId = request.id;
    bus_.publish(e);
}

void MaiEventEmitter::emitPermissionReplied(const MaiPermissionRequest& request,
                                            MaiPermissionDecision decision) {
    MaiEvent e = makeEvent(MaiEventType::PermissionReplied, request.sessionId);
    e.messageId = request.messageId;
    e.partId = request.partId;
    e.permissionId = request.id;
    e.detail = maiPermissionDecisionToString(decision);
    bus_.publish(e);
}
