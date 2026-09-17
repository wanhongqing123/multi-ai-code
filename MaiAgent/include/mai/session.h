#pragma once
#include <string>

#include "mai/types.h"

namespace mai {

// 纯数据。会话本身不知道自己怎么被存、怎么被跑。
struct Session {
  std::string id;  // ses_...
  std::string title;
  std::string directory;
  std::string model;           // 空表示用 Agent 的默认模型
  std::string agent = "build";
  Millis created = 0;
  Millis updated = 0;

  // 标题是不是还没被真正命名过。第一轮结束后会用用户那句话填上。
  bool untitled() const { return title.empty() || title == "新会话"; }
};

}  // namespace mai
