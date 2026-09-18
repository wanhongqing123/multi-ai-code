#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "MaiModelClient.h"

// 按剧本应答的假模型。**不开 socket、不起线程、不占端口。**
//
// 为什么不用假 HTTP 服务端：
//
// MaiModelClient 本来就是个抽象接口，MaiAgent 构造时接的就是它。测 agent
// 的行为（一轮的生命周期、工具循环、权限闸门）根本不需要经过传输层。
// 以前那三个测试各自起一个 httplib 服务端，于是每条断言都要走
//   组装 MaiModelRequest -> 序列化成 JSON -> TCP -> curl -> SSE 解析 -> 回调
// 一圈。想断言"第二轮带上了上一轮的回答"，得去 JSON 里翻
// `body["messages"][1]["content"]`——而那句话本来就在 MaiModelRequest
// 这个结构体里放着。
//
// 换成直接实现接口之后：
//   - 断言写 `request.messages[1].role == MaiModelRole::Assistant`，
//     编译器帮着查类型，不用对着字符串猜字段名
//   - 没有端口、线程和 sleep，不会因为机器慢就偶发失败
//   - 传输层的事留给真正该测它的地方（MaiModelClientTests 走真 socket，
//     e2e 走真 bridge）
//
// codex 也是这么分的：它的 core 集成测试用 wiremock 起真 HTTP，
// 而 line_buffer 那种纯解析逻辑是直接喂字节的单元测试
// （codex-rs/ollama/src/line_buffer_tests.rs）。
class MaiFakeModelClient final : public MaiModelClient {
public:
    // 一次应答的剧本。
    struct Turn {
        // 分片吐出的正文。分片是为了让上层能收到多条 delta 事件——
        // 界面就是靠这些增量渲染的，一次给全就测不到拼接。
        std::vector<std::string> textChunks;
        std::string reasoning;
        std::vector<MaiToolInvocation> invocations;

        // 非空表示这次应答以失败收场（网络断了、额度不足之类）。
        MaiError error;

        // 每个分片之间停多久。只在需要"还在跑"这个窗口的用例里设
        // （测 busy 拒绝、测中断），默认 0。
        int chunkDelayMilliseconds = 0;
    };

    MaiFakeModelClient() = default;
    explicit MaiFakeModelClient(std::vector<Turn> script);

    // 不按序号取剧本，每次请求都回同一个。
    //
    // 并发用例要这个：4 个会话同时问，按序号取的话它们会各拿到剧本里
    // 不同的一条，而那个用例想验证的恰恰是"4 个都拿到一样的完整回答"。
    void setRepeatingTurn(Turn turn);

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override;
    MaiWireApi wireApi() const override;

    // ── 回看收到过什么 ──────────────────────────────────────────
    // 存的是结构体原样，不是 JSON——断言直接对着字段写。
    std::size_t requestCount() const;
    MaiModelRequest request(std::size_t index) const;
    MaiModelRequest lastRequest() const;

private:
    // 剧本是共享状态：同一个客户端会被连着调好几次（工具循环里一轮有多次
    // 请求），而这些调用可能来自不同线程。
    mutable std::mutex mMutex;
    std::vector<Turn> mScript;
    Turn mRepeatingTurn;
    bool mRepeat = false;
    std::vector<MaiModelRequest> mRequests;
};
