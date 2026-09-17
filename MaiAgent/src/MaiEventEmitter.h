#pragma once
#include <string>
#include <string_view>

#include "MaiEventBus.h"
#include "MaiTime.h"
#include "MaiError.h"

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

private:
    MaiEventBus& bus_;
};
