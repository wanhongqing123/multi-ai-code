#include "MaiEventEmitter.h"

#include "MaiIdGenerator.h"

MaiEventEmitter::MaiEventEmitter(MaiEventBus& bus) : mBus(bus) {}

namespace {

// 所有事件共有的字段只在这一处填。加新事件类型时不会漏掉 id 或 time——
// 漏了不会编译报错，只会在界面上表现成某些更新莫名其妙丢了。
MaiEvent makeEvent(MaiEventType type, const std::string& sessionId) {
    MaiEvent event;
    event.id = MaiIdGenerator::newEventId();
    event.type = type;
    event.sessionId = sessionId;
    event.time = MaiTime::getCurrentTime();
    return event;
}

}  // namespace

void MaiEventEmitter::emitSession(MaiEventType type, const std::string& sessionId,
                                  const std::string& detail) {
    MaiEvent event = makeEvent(type, sessionId);
    event.detail = detail;
    mBus.publish(event);
}

void MaiEventEmitter::emitMessage(MaiEventType type, const std::string& sessionId,
                                  const std::string& messageId) {
    MaiEvent event = makeEvent(type, sessionId);
    event.messageId = messageId;
    mBus.publish(event);
}

void MaiEventEmitter::emitPart(MaiEventType type, const std::string& sessionId,
                               const std::string& messageId, const std::string& partId) {
    MaiEvent event = makeEvent(type, sessionId);
    event.messageId = messageId;
    event.partId = partId;
    mBus.publish(event);
}

void MaiEventEmitter::emitDelta(const std::string& sessionId, const std::string& messageId,
                                const std::string& partId, const char* field,
                                std::string_view chunk) {
    MaiEvent event = makeEvent(MaiEventType::MessagePartDelta, sessionId);
    event.messageId = messageId;
    event.partId = partId;
    event.field = field;
    event.delta.assign(chunk);
    mBus.publish(event);
}

void MaiEventEmitter::emitPermissionAsked(const MaiPermissionRequest& request) {
    MaiEvent event = makeEvent(MaiEventType::PermissionAsked, request.sessionId);
    event.messageId = request.messageId;
    event.partId = request.partId;
    event.permissionId = request.id;
    mBus.publish(event);
}

void MaiEventEmitter::emitPermissionReplied(const MaiPermissionRequest& request,
                                            MaiPermissionDecision decision) {
    MaiEvent event = makeEvent(MaiEventType::PermissionReplied, request.sessionId);
    event.messageId = request.messageId;
    event.partId = request.partId;
    event.permissionId = request.id;
    event.detail = maiPermissionDecisionToString(decision);
    mBus.publish(event);
}
