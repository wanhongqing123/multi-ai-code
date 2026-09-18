#pragma once
#include <string>
#include <string_view>

#include "MaiEventBus.h"
#include "MaiError.h"
#include "MaiPermission.h"
#include "MaiTime.h"

// 把"构造事件并发布"这件事收在一处。
//
// 之前 MaiAgent::Impl 里散着四个发事件的私有方法，每个都在手工填
// id/time/type 那几个字段——加一种事件就要再抄一遍，抄漏一个字段
// 不会编译报错，只会在界面上表现成某些更新丢了。
//
// 方法名都以 emit 开头：不带的话 `emitter->session(...)` 读起来
// 像在取 session，而它其实是在发事件。
class MaiEventEmitter {
public:
    explicit MaiEventEmitter(MaiEventBus& bus);

    void emitSession(MaiEventType type, const std::string& sessionId,
                     const std::string& detail = {});

    void emitMessage(MaiEventType type, const std::string& sessionId, const std::string& messageId);

    void emitPart(MaiEventType type, const std::string& sessionId, const std::string& messageId,
                  const std::string& partId);

    // 增量。**只带这次新增的内容**，不重推全量——
    // 流式期间每秒几十条，全量重推是纯烧 CPU。
    void emitDelta(const std::string& sessionId, const std::string& messageId,
                   const std::string& partId, const char* field, std::string_view chunk);

    // 有一次工具调用在等用户点头。partId 指向那个工具 part，
    // 界面从 part 上就能拿到工具名和参数，不用这个事件再带一份。
    void emitPermissionAsked(const MaiPermissionRequest& request);

    // 裁决落地了。界面据此把对话框关掉——**包括不是它发起的那次裁决**，
    // 多端同时开着的时候这一条是唯一的同步手段。
    void emitPermissionReplied(const MaiPermissionRequest& request, MaiPermissionDecision decision);

private:
    MaiEventBus& bus_;
};
