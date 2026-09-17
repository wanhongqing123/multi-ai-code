#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "MaiEventBus.h"
#include "MaiModelClient.h"
#include "MaiMessage.h"
#include "MaiSessionStore.h"
#include "MaiTool.h"
#include "MaiTime.h"
#include "MaiError.h"

#include "MaiContextBuilder.h"
#include "MaiEventEmitter.h"
#include "MaiSessionTitler.h"

// 跑一轮对话：组装上下文 → 流式请求 → 把增量变成事件 → 有工具调用就执行
// 并再来一圈 → 落库。
//
// 从 MaiAgent 里分出来的理由是职责：MaiAgent 是门面（接 MaiOperation、转发查询、持有依赖），
// 一轮对话的生命周期是另一回事。M4 的权限挂起也长在这里。
//
// 一个实例只跑一轮，跑完就扔——没有可复用的状态，也就没有"上一轮残留"这类 bug。
class MaiTurnRunner {
public:
    struct Deps {
        MaiSessionStore* store = nullptr;
        MaiModelClient* model = nullptr;
        MaiEventEmitter* emitter = nullptr;
        const MaiContextBuilder* context = nullptr;
        const MaiToolRegistry* tools = nullptr;
        const MaiSessionTitler* titler = nullptr;
        std::string defaultModel;
        // 模型可以连着调工具，一轮对话因此会有多次请求。设上限是因为模型会绕圈——
        // 拿同样的参数反复调同一个工具，没有上限就一直烧钱。
        int maxIterations = 12;
    };

    MaiTurnRunner(Deps deps, std::string sessionId, MaiMessage assistant);

    // 阻塞跑完一轮。调用方负责把它放到自己的线程上。
    void run(const std::atomic<bool>& cancel);

private:
    // 发一次请求并收完这一次的流。返回模型这次要调的工具（可能为空）。
    // 一轮对话里这个会被调用多次——每次工具执行完都要再问一遍模型。
    std::vector<MaiToolInvocation> requestCompletion(const MaiModelRequest& req,
                                                     const std::atomic<bool>& cancel);

    // 执行一批工具调用，把每个的结果作为 part 追加到 assistant_ 上。
    void executeTools(const std::vector<MaiToolInvocation>& calls, const std::atomic<bool>& cancel);

    // 组装这一次要发给模型的请求。
    MaiModelRequest buildRequest(const std::string& modelName) const;

    // 把这一圈累积的文本和推理落成 part。
    void commitStreamedParts();
    void finish(const std::atomic<bool>& cancel);

    Deps deps_;
    std::string session_id_;
    MaiMessage assistant_;

    // 每一圈的文本和推理各自是独立的 part——模型在工具调用前后说的话
    // 是两段不同的发言，混成一个 part 会让界面把工具卡夹在一段文字中间。
    std::string text_;
    std::string reasoning_;
    std::string text_part_id_;
    std::string reasoning_part_id_;

    MaiError error_;
};
