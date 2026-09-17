#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace mai {

using Millis = std::int64_t;
Millis now_ms();

// 错误码。之前全靠字符串传错误，调用方只能靠 substring 判断出了什么事——
// 界面要区分"没配模型"和"网络断了"就没办法了。
enum class ErrorCode {
  Ok,
  NotFound,       // 会话/消息不存在
  Busy,           // 这个会话已经有一轮在跑
  InvalidInput,   // 参数不合法（空 prompt 等）
  NotConfigured,  // 没有配模型客户端
  Network,        // 连不上、超时、TLS
  Protocol,       // 对端返回了看不懂的东西
  Canceled,       // 用户主动中断，**不是故障**
  Internal,
};

const char* to_string(ErrorCode c);

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;

  explicit operator bool() const { return code != ErrorCode::Ok; }
  static Error ok() { return {}; }
  static Error make(ErrorCode c, std::string msg) { return Error{c, std::move(msg)}; }
};

// 轻量 Result。C++17 没有 std::expected，自己兜一个够用的。
// 刻意不做成万能容器：只支持"有值"或"有错"，避免引入一堆模板复杂度。
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}                    // NOLINT: 允许隐式
  Result(Error error) : error_(std::move(error)) {}                // NOLINT
  Result(ErrorCode c, std::string msg) : error_{c, std::move(msg)} {}

  bool ok() const { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const { return ok(); }

  const T& value() const { return value_; }
  T& value() { return value_; }
  const Error& error() const { return error_; }

 private:
  T value_{};
  Error error_;
};

}  // namespace mai
