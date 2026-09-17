#pragma once

#include <string>

#include "MaiTime.h"

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
