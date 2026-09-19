#include "MaiTurnRunner.h"

#include "MaiIdGenerator.h"

namespace {

// 拒绝之后回灌给模型的话。
//
// "不要重试"这句是必须的：不写的话模型会拿一模一样的参数再调一次，
// 把 maxIterations 那 12 圈全烧在同一个被拒的操作上，用户看到的是
// agent 卡住了。
constexpr const char* kDeniedHint =
    "The user denied this tool call. Do not retry the same operation. "
    "Try a different approach, or ask the user how they want to proceed.";

// 超时和被拒绝要分开说。合在一起的话，模型会以为用户看过并且说了"不行"，
// 于是它会去"换个做法"——而真相是没人在，换什么做法都一样没人批。
constexpr const char* kTimedOutHint =
    "The approval request timed out with no answer from the user. "
    "Do not retry; tell the user what you were about to do and wait for them.";

}  // namespace

MaiTurnRunner::MaiTurnRunner(Dependencies dependencies, std::string sessionId, MaiMessage assistant)
    : mDependencies(std::move(dependencies)),
      mSessionId(std::move(sessionId)),
      mAssistant(std::move(assistant)) {}

void MaiTurnRunner::run(const std::atomic<bool>& cancel) {
    if (!mDependencies.model) {
        mError = MaiError::make(MaiErrorCode::NotConfigured, "no model client configured");
        finish(cancel);
        return;
    }

    MaiSession session;
    if (!mDependencies.store->getSession(mSessionId, session)) {
        mError = MaiError::make(MaiErrorCode::NotFound, "session not found");
        finish(cancel);
        return;
    }

    const std::string modelName =
        session.model.empty() ? mDependencies.defaultModel : session.model;

    // ── 工具循环 ──────────────────────────────────────────────────
    // 这是 agent 之所以是 agent 的地方：模型说要调工具 → 我们执行 → 把结果
    // 回灌 → 再问一次 → 它可能还要调 → 直到它不再要调为止。
    //
    // 每一圈都重新组装上下文，因为上一圈的工具结果已经作为 part 落在
    // mAssistant 上了，MaiContextBuilder 会把它展开成模型认得的形状。
    for (int iteration = 0; iteration < mDependencies.maxIterations; ++iteration) {
        if (cancel.load(std::memory_order_relaxed)) break;

        const auto calls = requestCompletion(buildRequest(modelName), cancel);
        commitStreamedParts();

        if (mError) break;
        if (calls.empty()) break;  // 模型不再要调工具，这一轮结束
        if (cancel.load(std::memory_order_relaxed)) break;

        executeTools(calls, cancel);

        // 到达上限还没收手：明确告诉用户，而不是悄悄停在半路让人以为跑完了。
        if (iteration + 1 >= mDependencies.maxIterations) {
            mError = MaiError::make(MaiErrorCode::Internal,
                                    "Stopped after reaching the tool-call limit of " +
                                        std::to_string(mDependencies.maxIterations) +
                                        " iterations. Ask me to try a different approach.");
        }
    }

    finish(cancel);
}

MaiModelRequest MaiTurnRunner::buildRequest(const std::string& modelName) const {
    MaiModelRequest request;
    request.model = modelName;

    // 历史里那条正在写的 assistant 消息，要用内存中最新的版本——
    // 存储里的那份可能还没包含刚执行完的工具结果。
    auto history = mDependencies.store->listMessages(mSessionId);
    for (auto& message : history) {
        if (message.id == mAssistant.id) message = mAssistant;
    }
    request.messages = mDependencies.context->build(history);

    if (mDependencies.tools && !mDependencies.tools->isEmpty())
        request.tools = mDependencies.tools->specs();
    return request;
}

std::vector<MaiToolInvocation> MaiTurnRunner::requestCompletion(const MaiModelRequest& request,
                                                                const std::atomic<bool>& cancel) {
    // 每一圈的文本是独立的 part：模型在调工具前后说的话是两段发言，
    // 混成一个 part 会让界面把工具卡夹在一段文字中间。
    mText.clear();
    mReasoning.clear();
    mTextPartId = MaiIdGenerator::newPartId();
    mReasoningPartId = MaiIdGenerator::newPartId();

    bool textStarted = false;
    bool reasoningStarted = false;
    std::vector<MaiToolInvocation> calls;

    MaiStreamSink sink;
    sink.onText = [&](std::string_view chunk) {
        if (!textStarted) {
            textStarted = true;
            mDependencies.emitter->emitPart(MaiEventType::MessagePartUpdated, mSessionId,
                                            mAssistant.id, mTextPartId);
        }
        mText.append(chunk);
        mDependencies.emitter->emitDelta(mSessionId, mAssistant.id, mTextPartId, "text", chunk);
    };
    sink.onReasoning = [&](std::string_view chunk) {
        if (!reasoningStarted) {
            reasoningStarted = true;
            mDependencies.emitter->emitPart(MaiEventType::MessagePartUpdated, mSessionId,
                                            mAssistant.id, mReasoningPartId);
        }
        mReasoning.append(chunk);
        mDependencies.emitter->emitDelta(mSessionId, mAssistant.id, mReasoningPartId, "text",
                                         chunk);
    };
    sink.onToolCall = [&](const MaiToolInvocation& call) { calls.push_back(call); };

    mError = mDependencies.model->stream(request, sink, cancel);
    return calls;
}

void MaiTurnRunner::executeTools(const std::vector<MaiToolInvocation>& calls,
                                 const std::atomic<bool>& cancel) {
    MaiSession session;
    mDependencies.store->getSession(mSessionId, session);

    MaiToolContext context;
    context.sessionId = mSessionId;
    context.root = session.directory;
    context.cancel = &cancel;

    for (const auto& call : calls) {
        if (cancel.load(std::memory_order_relaxed)) break;

        MaiMessagePart part;
        part.id = MaiIdGenerator::newPartId();
        part.created = MaiTime::getCurrentTime();
        MaiToolPart body;
        body.tool = call.name;
        body.callId = call.id;
        body.input = call.arguments;
        // 要审批的工具先挂在 Pending：界面靠这个状态显示"等待授权"，
        // 不用另外对着 permission.asked 事件维护一张表。
        MaiTool* tool = mDependencies.tools ? mDependencies.tools->find(call.name) : nullptr;
        const bool needsApproval = tool && tool->requiresApproval();
        body.state = needsApproval ? MaiToolState::Pending : MaiToolState::Running;
        part.body = body;
        mAssistant.parts.push_back(part);

        // 先落库再广播。
        //
        // 顺序反了会有个不容易发现的洞：事件说"有个 part 更新了"，界面拿着
        // 这个 id 去 /message 拉全量，却什么都拉不到——因为那时还没入库。
        // 工具跑几秒是常事，等授权更是以分钟计，这个窗口期足够用户刷新一次。
        mDependencies.store->putMessage(mSessionId, mAssistant);
        mDependencies.emitter->emitPart(MaiEventType::MessagePartUpdated, mSessionId, mAssistant.id,
                                        part.id);

        MaiToolResult result;
        if (!tool) {
            // 模型有时会编一个不存在的工具名。告诉它事实，它下一圈通常会改对；
            // 直接失败整轮反而更糟。
            result = MaiToolResult::failure(MaiErrorCode::NotFound,
                                            "no tool named \"" + call.name + "\" is registered");
        } else if (context.root.empty()) {
            result = MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so file tools are unavailable");
        } else {
            bool allowed = true;
            MaiToolResult denial = checkPermission(call, part.id, allowed, cancel);
            if (!allowed) {
                result = std::move(denial);
            } else {
                if (needsApproval) {
                    // 批了才真正开跑。这条状态变化界面等着用——不发的话工具卡
                    // 会一直停在"等待授权"，直到它跑完才跳变。
                    std::get<MaiToolPart>(mAssistant.parts.back().body).state =
                        MaiToolState::Running;
                    mDependencies.store->putMessage(mSessionId, mAssistant);
                    mDependencies.emitter->emitPart(MaiEventType::MessagePartUpdated, mSessionId,
                                                    mAssistant.id, part.id);
                }
                result = tool->execute(call.arguments, context);
            }
        }

        auto& stored = std::get<MaiToolPart>(mAssistant.parts.back().body);
        if (result.hasError()) {
            stored.state = MaiToolState::Error;
            stored.error = result.error().message();
            // 错误也要回灌给模型——它需要知道失败了才能换个做法。
            stored.output = result.error().message();
        } else {
            stored.state = MaiToolState::Completed;
            stored.output = result.output();
            if (result.isTruncated()) stored.output += "\n(output truncated)";
        }
        mDependencies.emitter->emitPart(MaiEventType::MessagePartUpdated, mSessionId, mAssistant.id,
                                        part.id);

        // 每执行完一个工具就落一次库：工具可能跑很久，中途崩了不该丢掉已完成的部分。
        mDependencies.store->putMessage(mSessionId, mAssistant);
    }
}

MaiToolResult MaiTurnRunner::checkPermission(const MaiToolInvocation& call,
                                             const std::string& partId, bool& allowed,
                                             const std::atomic<bool>& cancel) {
    allowed = true;
    MaiTool* tool = mDependencies.tools ? mDependencies.tools->find(call.name) : nullptr;
    if (!tool || !tool->requiresApproval()) return {};

    // 模型被拒之后经常原样再试一次。第二次不再弹框，直接回同样的话。
    const std::string signature = call.name + std::string(1, '\0') + call.arguments;
    if (mRejected.count(signature) > 0) {
        allowed = false;
        return MaiToolResult::failure(MaiErrorCode::Canceled, kDeniedHint);
    }

    if (!mDependencies.permissions) {
        // 没装闸门 = 没有任何人能点头。兜底成拒绝而不是放行：
        // 放行意味着"忘了接权限"这个疏忽会静默地让模型改用户的文件。
        allowed = false;
        mRejected.insert(signature);
        return MaiToolResult::failure(
            MaiErrorCode::NotConfigured,
            "This tool requires user approval, but no permission gate is wired up.");
    }

    // 用户之前对这个会话说过"以后都允许"。
    if (mDependencies.permissions->isAllowedInSession(mSessionId, call.name)) return {};

    MaiPermissionRequest request;
    request.id = MaiIdGenerator::newPermissionId();
    request.sessionId = mSessionId;
    request.messageId = mAssistant.id;
    request.partId = partId;
    request.toolName = call.name;
    request.arguments = call.arguments;
    request.asked = MaiTime::getCurrentTime();

    MaiEventEmitter* emitter = mDependencies.emitter;
    // ask() 会阻塞这个线程直到有人裁决。每一轮跑在自己的线程上，
    // 所以这里卡住不影响别的会话（见 MaiPermission.h 的线程契约）。
    const MaiPermissionDecision decision = mDependencies.permissions->ask(
        request,
        [emitter](const MaiPermissionRequest& request) {
            if (emitter) emitter->emitPermissionAsked(request);
        },
        cancel);

    if (emitter) emitter->emitPermissionReplied(request, decision);

    if (decision == MaiPermissionDecision::TimedOut) {
        allowed = false;
        mRejected.insert(signature);
        return MaiToolResult::failure(MaiErrorCode::Canceled, kTimedOutHint);
    }

    if (decision == MaiPermissionDecision::Denied) {
        allowed = false;
        mRejected.insert(signature);
        // 中断和拒绝在闸门那边长得一样（都是 Reject），但对模型说的话要分开：
        // 中断时整轮马上就结束了，那句"换个做法"没人会读到，只会被存进历史
        // 误导下一轮。
        if (cancel.load(std::memory_order_relaxed))
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Interrupted.");
        return MaiToolResult::failure(MaiErrorCode::Canceled, kDeniedHint);
    }
    return {};
}

void MaiTurnRunner::commitStreamedParts() {
    // 落库只在这里做。每个 delta 落一次盘等于每秒几十次 fsync。
    if (!mReasoning.empty()) {
        MaiMessagePart part;
        part.id = mReasoningPartId;
        part.body = MaiReasoningPart{mReasoning};
        part.created = MaiTime::getCurrentTime();
        mAssistant.parts.push_back(std::move(part));
        mReasoning.clear();
    }
    if (!mText.empty()) {
        MaiMessagePart part;
        part.id = mTextPartId;
        part.body = MaiTextPart{mText};
        part.created = MaiTime::getCurrentTime();
        mAssistant.parts.push_back(std::move(part));
        mText.clear();
    }
}

void MaiTurnRunner::finish(const std::atomic<bool>& cancel) {
    commitStreamedParts();  // 中断时可能还有没落库的半截内容

    mAssistant.completed = MaiTime::getCurrentTime();
    mDependencies.store->putMessage(mSessionId, mAssistant);
    mDependencies.emitter->emitMessage(MaiEventType::MessageUpdated, mSessionId, mAssistant.id);

    // 给会话起名不是"跑一轮"的职责，委托出去（见 session_titler.h）。
    if (mDependencies.titler) {
        const std::string title = mDependencies.titler->apply(*mDependencies.store, mSessionId);
        if (!title.empty()) {
            mDependencies.emitter->emitSession(MaiEventType::SessionUpdated, mSessionId, title);
        }
    }

    // 落库失败不能无声无息。磁盘满了还假装存上了，用户是第二天打开
    // 发现对话没了才知道的。一轮查一次就够——写接口在热路径上，
    // 每次都检查会把代码淹掉（见 MaiSessionStore::lastWriteError）。
    const MaiError writeError = mDependencies.store->lastWriteError();
    if (writeError.hasError() && !mError.hasError()) mError = writeError;

    // 主动中断不是故障：界面不该弹错误，已经吐出来的内容也照常保留。
    const bool canceled =
        cancel.load(std::memory_order_relaxed) || mError.code() == MaiErrorCode::Canceled;
    if (mError && !canceled) {
        mDependencies.emitter->emitSession(MaiEventType::SessionError, mSessionId,
                                           mError.message());
    }
}
