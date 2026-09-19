#pragma once

#include <string>
#include <utility>

// 错误表示。整个工程只用这一套，不抛异常。
//
// ── 为什么不用异常 ──────────────────────────────────────────────
// 这个库要被链进 Qt 桌面、iOS / Android、嵌入式。那些环境里异常要么被关掉（-fno-exceptions），
// 要么跨 ABI 边界传不过去。返回值到处都能用。
//
// ── 为什么要错误码，光有字符串不行 ──────────────────────────────
// 这一版之前全靠字符串传错误，调用方只能靠 substring 去猜出了什么事。
// 界面想区分"没配模型"（该弹设置页）和"网络断了"（该提示重试）就没办法。
//
// 现在两样都有：**码给机器看，文字给人看**。

// 错误码。
//
// 刻意做得很粗，不做成每种失败一个码——码的用途是让调用方**分支**，
// 分不出不同处理方式的失败就该归到同一个码里，细节留在 message。
enum class MaiErrorCode {
    Ok,             // 没出错。MaiError 默认构造就是这个
    NotFound,       // 会话 / 消息 / 文件 / 授权请求不存在。HTTP 404
    Busy,           // 这个会话已经有一轮在跑。HTTP 409
    InvalidInput,   // 参数不合法：空 prompt、越界路径、认不出的 decision。HTTP 400
    NotConfigured,  // 没有配模型客户端，或者鉴权没通过。HTTP 503
    Network,        // 连不上、超时、TLS 握手失败。HTTP 502
    Protocol,       // 对端返回了看不懂的东西（不是合法 SSE、缺字段）。HTTP 502
    // 用户主动中断，或者授权超时。**这不是故障**——界面不该弹错误，已经吐出来的内容也要照常保留。
    // MaiTurnRunner::finish 专门判了这一条。
    Canceled,
    Internal,  // 剩下的。落库失败、curl 初始化失败之类
};

// 转成 "not_found" 这类稳定字符串。给日志和跨进程传递用——接收方据此分支，
// 而不是去匹配 message 的文字（文案会改，码不会）。
const char* maiErrorCodeToString(MaiErrorCode code);

// 一个错误，或者"没有错误"。
//
// 默认构造出来是**没有错误**的状态，所以函数里 `return {};` 就表示成功。
class MaiError {
public:
    MaiError() = default;
    MaiError(MaiErrorCode code, std::string message);

    static MaiError ok();
    static MaiError make(MaiErrorCode code, std::string message);

    MaiErrorCode code() const;
    // 给人看的。不要拿它做逻辑判断——文案会改，判断要用 code()。
    const std::string& message() const;
    bool hasError() const;

    // 方便写 `if (error) { ... }`。
    //
    // 注意这里的方向：**有错时为真**，和 MaiResult 正好相反（那边是"成功时为真"）。
    // 两个类型混着用的地方容易看岔，拿不准就写 hasError() / isOk()，别图短。
    explicit operator bool() const;

private:
    MaiErrorCode mCode = MaiErrorCode::Ok;
    std::string mMessage;
};

// 轻量 Result：要么有值，要么有错。
//
// C++17 没有 std::expected，自己兜一个够用的。刻意**不**做成万能容器：
// 不支持 map / and_then 那套，也不管引用类型和 void。那些会引入一堆模板复杂度，
// 而这个工程里的用法就三种——submit 的返回、打开数据库、以后可能的工厂函数。
//
// T 必须能默认构造：出错时 mValue 是默认构造出来的，读它没有意义但也不会是未定义行为。
//
// 这是编码规范第 5 节说的**例外**：模板的实例化需要看得见定义，所以实现只能留在头文件里。
template <typename T>
class MaiResult {
public:
    // 两个隐式构造是有意的：函数里直接 `return someValue;` 或者
    // `return {MaiErrorCode::NotFound, "..."};` 都能写，不用每次点名类型。
    MaiResult(T value) : mValue(std::move(value)) {}         // NOLINT: 允许隐式
    MaiResult(MaiError error) : mError(std::move(error)) {}  // NOLINT
    MaiResult(MaiErrorCode code, std::string message) : mError(code, std::move(message)) {}

    bool isOk() const {
        return !mError.hasError();
    }
    // **成功时为真**，和 MaiError::operator bool 方向相反。
    explicit operator bool() const {
        return isOk();
    }

    // 出错时返回的是默认构造的值，不是未定义行为——但读它没有意义。先 isOk() 再取值。
    const T& value() const {
        return mValue;
    }
    T& value() {
        return mValue;
    }
    // T 是只能移动的类型（比如 unique_ptr）时，这么取：
    //   auto store = std::move(result.value());
    const MaiError& error() const {
        return mError;
    }

private:
    T mValue{};
    MaiError mError;
};
