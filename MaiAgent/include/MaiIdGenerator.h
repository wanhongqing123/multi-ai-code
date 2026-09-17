#pragma once

#include <string>

// ID 生成。
//
// 前缀沿用 opencode 的约定，因为现有 Electron 界面会按前缀做校验：
// openapi.json 里 sessionID 是 ^ses、messageID 是 ^msg、partID 是 ^prt、
// 事件 id 是 ^evt_。第一阶段要让界面不改一行就能接上，所以必须照它的来。
class MaiIdGenerator {
public:
    static std::string newSessionId();     // ses_...
    static std::string newMessageId();     // msg_...
    static std::string newPartId();        // prt_...
    static std::string newEventId();       // evt_...
    static std::string newPermissionId();  // per_...

    // 单调递增 + 随机后缀。单调是为了让同一毫秒内产生的 id 仍可按字典序
    // 排序——消息和 part 的显示顺序直接依赖这个，纯随机 id 会导致刷新后
    // 顺序乱跳。
    static std::string generate(const char* prefix);
};
