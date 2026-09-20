#pragma once

#include <cstdint>
#include <string>

// Unix 毫秒时间戳。
using MaiMillis = std::int64_t;

// 时间相关的工具。
//
// 文件名叫 MaiTime.h 而不是 time.h —— 后者是 C 标准库的头，
// 同名会在某些 include 路径顺序下直接撞上。
class MaiTime {
public:
    static MaiMillis getCurrentTime();

    // 转成 ISO 8601 的 UTC 写法，像 "2026-09-20T15:04:05Z"。
    //
    // **固定用 UTC**，不用本地时间：字符串要能自己说清是哪个时区，
    // 不带时区的时间戳传到别处（日志、模型上下文、另一台机器）就没法解释了。
    // codex 的 current_time 也是只给 UTC。
    static std::string formatIso8601Utc(MaiMillis millis);
};
