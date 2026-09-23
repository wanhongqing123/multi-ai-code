#pragma once
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "MaiEventBus.h"
#include "MaiModelClient.h"
#include "MaiMessage.h"
#include "MaiPermission.h"
#include "MaiSessionStore.h"
#include "MaiTool.h"
#include "MaiTime.h"
#include "MaiError.h"

#include "MaiContextBuilder.h"
#include "MaiEventEmitter.h"
#include "MaiSessionTitler.h"

// 跑一轮对话：组装上下文 → 流式请求 → 把增量变成事件 → 有工具调用就执行并再来一圈 → 落库。
//
// 从 MaiAgent 里分出来的理由是职责：MaiAgent 是门面（接 MaiOperation、转发查询、持有依赖），
// 一轮对话的生命周期是另一回事。M4 的权限挂起也长在这里。
//
// 一个实例只跑一轮，跑完就扔——没有可复用的状态，也就没有"上一轮残留"这类 bug。
class MaiTurnRunner {
public:
    struct Dependencies {
        MaiSessionStore* store = nullptr;
        MaiModelClient* model = nullptr;
        MaiEventEmitter* emitter = nullptr;
        const MaiContextBuilder* context = nullptr;
        const MaiToolRegistry* tools = nullptr;
        const MaiSessionTitler* titler = nullptr;
        // 可以为空：那样 requiresApproval() 的工具一律直接拒绝。
        // 不是"直接放行"——没装闸门就等于没人能点头，而不是所有人都点了头。
        MaiPermissionGate* permissions = nullptr;
        // 可以为空：那样 question 工具会明说「这里问不了」，而不是干等。
        MaiQuestionGate* questions = nullptr;
        // 可以为空：那样子 Agent 那组工具会明说「这儿起不了」。
        MaiSubAgentHost* subAgents = nullptr;
        std::string defaultModel;
        // 模型可以连着调工具，一轮对话因此会有多次请求。
        // 设上限是因为模型会绕圈——拿同样的参数反复调同一个工具，没有上限就一直烧钱。
        int maxIterations = 12;
        MaiApprovalPolicy approvalPolicy = MaiApprovalPolicy::OnRequest;
    };

    MaiTurnRunner(Dependencies dependencies, std::string sessionId, MaiMessage assistant);

    // 阻塞跑完一轮。调用方负责把它放到自己的线程上。
    void run(const std::atomic<bool>& cancel);

private:
    // 发一次请求并收完这一次的流。返回模型这次要调的工具（可能为空）。
    // 一轮对话里这个会被调用多次——每次工具执行完都要再问一遍模型。
    std::vector<MaiToolInvocation> requestCompletion(const MaiModelRequest& request,
                                                     const std::atomic<bool>& cancel);

    // 执行一批工具调用，把每个的结果作为 part 追加到 mAssistant 上。
    void executeTools(const std::vector<MaiToolInvocation>& calls, const std::atomic<bool>& cancel);

    // 这次调用该不该放行。需要审批的话会在这里阻塞等用户裁决。返回空表示放行；
    // 非空就是要直接回灌给模型的失败结果。
    MaiToolResult checkPermission(const MaiToolInvocation& call, const std::string& partId,
                                  bool& allowed, const std::atomic<bool>& cancel);
    bool toolNeedsApproval(const MaiToolInvocation& call) const;

    // 组装这一次要发给模型的请求。
    MaiModelRequest buildRequest(const std::string& modelName, bool requireFinalAnswer) const;

    // 流式 part 刚冒头时：先在消息里占个位、落库，再广播。
    //
    // 顺序和工具那边一样，理由也一样（规范第 10 节）：事件只带 part id，订阅方要拿它去存储查全量，
    // 库里没有就什么都查不到。
    //
    // **这一条的代价比工具那边隐蔽得多。** 正文和思考过程的增量 field 都是 "text"，
    // 订阅方唯一能分清的办法就是去查这个 part 是 MaiTextPart 还是 MaiReasoningPart。
    // 查不到就只能按正文渲染——于是模型的思考过程被当成答案打给用户看。
    // 这个洞是拿真实 GLM-5.3 跑出来的：假模型不发 reasoning_content，测不到。
    void beginStreamedPart(const std::string& partId, MaiMessagePartBody body);

    // 把这一圈累积的文本和推理填进各自的 part。**就地填**，
    // 不是新建——part 在第一个 chunk 到达时就已经占位入库了。
    void commitStreamedParts();
    void fillStreamedPart(const std::string& partId, std::string& buffer);
    void finish(const std::atomic<bool>& cancel);

    Dependencies mDependencies;
    std::string mSessionId;
    MaiMessage mAssistant;

    // 每一圈的文本和推理各自是独立的 part——模型在工具调用前后说的话是两段不同的发言，
    // 混成一个 part 会让界面把工具卡夹在一段文字中间。
    std::string mText;
    std::string mReasoning;
    std::string mTextPartId;
    std::string mReasoningPartId;

    MaiError mError;

    // 这一轮里已经被拒绝过的 (工具名, 参数)。模型被拒之后经常原样再调一次，
    // 每次都弹一个框会把用户烦死；记下来直接回同样的拒绝，它才会换招。
    //
    // 只记这一轮：用户拒绝的是"现在这件事"，下一轮同样的请求该重新问。
    std::set<std::string> mRejected;
};
