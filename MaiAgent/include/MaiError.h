#pragma once

#include <string>
#include <utility>

// 错误码。
//
// 之前全靠字符串传错误，调用方只能靠 substring 判断出了什么事——
// 界面要区分"没配模型"和"网络断了"就没办法了。
enum class MaiErrorCode {
    Ok,
    NotFound,       // 会话 / 消息不存在
    Busy,           // 这个会话已经有一轮在跑
    InvalidInput,   // 参数不合法（空 prompt、越界路径等）
    NotConfigured,  // 没有配模型客户端，或鉴权没通过
    Network,        // 连不上、超时、TLS
    Protocol,       // 对端返回了看不懂的东西
    Canceled,       // 用户主动中断，**不是故障**
    Internal,
};

const char* maiErrorCodeToString(MaiErrorCode code);

class MaiError {
public:
    MaiError() = default;
    MaiError(MaiErrorCode code, std::string message);

    static MaiError ok();
    static MaiError make(MaiErrorCode code, std::string message);

    MaiErrorCode code() const;
    const std::string& message() const;
    bool hasError() const;

    explicit operator bool() const;

private:
    MaiErrorCode mCode = MaiErrorCode::Ok;
    std::string mMessage;
};

// 轻量 Result。C++17 没有 std::expected，自己兜一个够用的。
// 刻意不做成万能容器：只支持"有值"或"有错"，避免引入一堆模板复杂度。
//
// 这是编码规范里说的例外之一：模板的实例化需要看得见定义，
// 所以实现只能留在头文件里。
template <typename T>
class MaiResult {
public:
    MaiResult(T value) : mValue(std::move(value)) {}         // NOLINT: 允许隐式
    MaiResult(MaiError error) : mError(std::move(error)) {}  // NOLINT
    MaiResult(MaiErrorCode code, std::string message) : mError(code, std::move(message)) {}

    bool isOk() const {
        return !mError.hasError();
    }
    explicit operator bool() const {
        return isOk();
    }

    const T& value() const {
        return mValue;
    }
    T& value() {
        return mValue;
    }
    const MaiError& error() const {
        return mError;
    }

private:
    T mValue{};
    MaiError mError;
};
