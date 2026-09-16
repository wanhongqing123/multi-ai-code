#pragma once
#include <string>

namespace mai::agent {

// ID 前缀沿用 opencode 的约定，因为现有 Electron UI 会按这些前缀做校验：
// openapi.json 里 sessionID 是 ^ses、messageID 是 ^msg、partID 是 ^prt、事件 id 是 ^evt_。
// 第一阶段要让 UI 不改一行就能接上，所以这里必须照它的来。
namespace id {

std::string session();   // ses_xxxxxxxxxxxx
std::string message();   // msg_xxxxxxxxxxxx
std::string part();      // prt_xxxxxxxxxxxx
std::string event();     // evt_xxxxxxxxxxxx
std::string permission();

// 单调递增 + 随机后缀。单调是为了让同一毫秒内产生的 id 仍可按字典序排序——
// 消息和 part 的显示顺序直接依赖这个，用纯随机 id 会导致刷新后顺序乱跳。
std::string generate(const char* prefix);

}  // namespace id
}  // namespace mai::agent
