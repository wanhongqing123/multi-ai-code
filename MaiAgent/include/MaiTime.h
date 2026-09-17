#pragma once

#include <cstdint>

// Unix 毫秒时间戳。
using MaiMillis = std::int64_t;

// 时间相关的工具。
//
// 文件名叫 MaiTime.h 而不是 time.h —— 后者是 C 标准库的头，
// 同名会在某些 include 路径顺序下直接撞上。
class MaiTime {
public:
    static MaiMillis getCurrentTime();
};
