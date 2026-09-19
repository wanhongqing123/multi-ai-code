#pragma once

#include <string>

#include "MaiTime.h"

// 新建会话在被真正命名之前用的占位标题。
//
// 这里放英文而不是"新会话"：核心是个要被链进各种壳的库，本地化是界面的事。把中文写死在库里，
// 等于所有壳都被迫说中文。
//
// 收成常量是因为 isUntitled() 要拿它做相等比较，两处各写一份字面量的话，
// 改其中一处就会让"还没命名"这个判断静默失效。
inline constexpr const char* kMaiDefaultSessionTitle = "New session";

// 一个会话。纯数据：它不知道自己怎么被存、怎么被跑。
struct MaiSession {
    std::string id;  // ses_...
    std::string title;
    std::string directory;
    std::string model;  // 空表示用 MaiAgent 的默认模型
    std::string agent = "build";
    MaiMillis created = 0;
    MaiMillis updated = 0;

    // 标题是不是还没被真正命名过。第一轮结束后会用用户那句话填上。
    bool isUntitled() const;
};
