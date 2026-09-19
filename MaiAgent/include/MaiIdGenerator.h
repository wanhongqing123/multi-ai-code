#pragma once

#include <string>

// ID 生成。
//
// **带前缀不是我们挑的形状，是被现有界面逼的。** 那个 Electron 界面派生自 opencode，
// 它按前缀做校验，而且是两道：
// packages/sdk/openapi.json 里 sessionID / messageID / partID / 事件 id 分别有 ^ses、^msg、^prt、
// ^evt_的 pattern；另外服务端路由里还有一句
// `if (!payload.sessionID.startsWith("ses")) return BadRequest`。
// 第一阶段要让界面不改一行就能接上，所以只能照它的来。
//
// 我们的参照系是 codex，而 codex 这里的做法不一样：
// 它的 ThreadId 就是一个裸的 UUIDv7（codex-rs/protocol/src/thread_id.rs），不带前缀。
// UUIDv7 本身按时间有序，
// 所以它不需要我们下面这套"单调计数 + 随机后缀"——那套是在手工复刻 UUIDv7 已经保证的性质。
//
// 等界面不再依赖前缀（Qt 端直接链库，根本不过这层校验），这里就该换成 UUIDv7，
// 把这套手写的东西删掉。
class MaiIdGenerator {
public:
    static std::string newSessionId();     // ses_...
    static std::string newMessageId();     // msg_...
    static std::string newPartId();        // prt_...
    static std::string newEventId();       // evt_...
    static std::string newPermissionId();  // per_...

    // 单调递增 + 随机后缀。
    // 单调是为了让同一毫秒内产生的 id 仍可按字典序排序——消息和 part 的显示顺序直接依赖这个，
    // 纯随机 id 会导致刷新后顺序乱跳。
    static std::string generate(const char* prefix);
};
