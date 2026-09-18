#include "MaiTurnRunner.h"

#include "MaiIdGenerator.h"

namespace {

// 拒绝之后回灌给模型的话。
//
// "不要重试"这句是必须的：不写的话模型会拿一模一样的参数再调一次，
// 把 maxIterations 那 12 圈全烧在同一个被拒的操作上，用户看到的是
// agent 卡住了。
constexpr const char* kRejectedHint =
    "用户拒绝了这次调用。不要重试同样的操作，改用别的办法，或者直接问用户想怎么做。";

}  // namespace

MaiTurnRunner::MaiTurnRunner(Deps deps, std::string sessionId, MaiMessage assistant)
    : deps_(std::move(deps)), session_id_(std::move(sessionId)), assistant_(std::move(assistant)) {}

void MaiTurnRunner::run(const std::atomic<bool>& cancel) {
    if (!deps_.model) {
        error_ = MaiError::make(MaiErrorCode::NotConfigured, "没有配置模型客户端");
        finish(cancel);
        return;
    }

    MaiSession session;
    if (!deps_.store->getSession(session_id_, session)) {
        error_ = MaiError::make(MaiErrorCode::NotFound, "会话不存在");
        finish(cancel);
        return;
    }

    const std::string modelName = session.model.empty() ? deps_.defaultModel : session.model;

    // ── 工具循环 ──────────────────────────────────────────────────
    // 这是 agent 之所以是 agent 的地方：模型说要调工具 → 我们执行 → 把结果
    // 回灌 → 再问一次 → 它可能还要调 → 直到它不再要调为止。
    //
    // 每一圈都重新组装上下文，因为上一圈的工具结果已经作为 part 落在
    // assistant_ 上了，MaiContextBuilder 会把它展开成模型认得的形状。
    for (int iteration = 0; iteration < deps_.maxIterations; ++iteration) {
        if (cancel.load(std::memory_order_relaxed)) break;

        const auto calls = requestCompletion(buildRequest(modelName), cancel);
        commitStreamedParts();

        if (error_) break;
        if (calls.empty()) break;  // 模型不再要调工具，这一轮结束
        if (cancel.load(std::memory_order_relaxed)) break;

        executeTools(calls, cancel);

        // 到达上限还没收手：明确告诉用户，而不是悄悄停在半路让人以为跑完了。
        if (iteration + 1 >= deps_.maxIterations) {
            error_ = MaiError::make(MaiErrorCode::Internal,
                                    "工具调用达到上限（" + std::to_string(deps_.maxIterations) +
                                        " 轮）后停止。可以让我换个思路再试。");
        }
    }

    finish(cancel);
}

MaiModelRequest MaiTurnRunner::buildRequest(const std::string& modelName) const {
    MaiModelRequest req;
    req.model = modelName;

    // 历史里那条正在写的 assistant 消息，要用内存中最新的版本——
    // 存储里的那份可能还没包含刚执行完的工具结果。
    auto history = deps_.store->listMessages(session_id_);
    for (auto& m : history) {
        if (m.id == assistant_.id) m = assistant_;
    }
    req.messages = deps_.context->build(history);

    if (deps_.tools && !deps_.tools->isEmpty()) req.tools = deps_.tools->specs();
    return req;
}

std::vector<MaiToolInvocation> MaiTurnRunner::requestCompletion(const MaiModelRequest& req,
                                                                const std::atomic<bool>& cancel) {
    // 每一圈的文本是独立的 part：模型在调工具前后说的话是两段发言，
    // 混成一个 part 会让界面把工具卡夹在一段文字中间。
    text_.clear();
    reasoning_.clear();
    text_part_id_ = MaiIdGenerator::newPartId();
    reasoning_part_id_ = MaiIdGenerator::newPartId();

    bool textStarted = false;
    bool reasoningStarted = false;
    std::vector<MaiToolInvocation> calls;

    MaiStreamSink sink;
    sink.onText = [&](std::string_view chunk) {
        if (!textStarted) {
            textStarted = true;
            deps_.emitter->emitPart(MaiEventType::MessagePartUpdated, session_id_, assistant_.id,
                                    text_part_id_);
        }
        text_.append(chunk);
        deps_.emitter->emitDelta(session_id_, assistant_.id, text_part_id_, "text", chunk);
    };
    sink.onReasoning = [&](std::string_view chunk) {
        if (!reasoningStarted) {
            reasoningStarted = true;
            deps_.emitter->emitPart(MaiEventType::MessagePartUpdated, session_id_, assistant_.id,
                                    reasoning_part_id_);
        }
        reasoning_.append(chunk);
        deps_.emitter->emitDelta(session_id_, assistant_.id, reasoning_part_id_, "text", chunk);
    };
    sink.onToolCall = [&](const MaiToolInvocation& call) { calls.push_back(call); };

    error_ = deps_.model->stream(req, sink, cancel);
    return calls;
}

void MaiTurnRunner::executeTools(const std::vector<MaiToolInvocation>& calls,
                                 const std::atomic<bool>& cancel) {
    MaiSession session;
    deps_.store->getSession(session_id_, session);

    MaiToolContext ctx;
    ctx.sessionId = session_id_;
    ctx.root = session.directory;
    ctx.cancel = &cancel;

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
        MaiTool* tool = deps_.tools ? deps_.tools->find(call.name) : nullptr;
        const bool needsApproval = tool && tool->requiresApproval();
        body.state = needsApproval ? MaiToolState::Pending : MaiToolState::Running;
        part.body = body;
        assistant_.parts.push_back(part);

        // 先落库再广播。
        //
        // 顺序反了会有个不容易发现的洞：事件说"有个 part 更新了"，界面拿着
        // 这个 id 去 /message 拉全量，却什么都拉不到——因为那时还没入库。
        // 工具跑几秒是常事，等授权更是以分钟计，这个窗口期足够用户刷新一次。
        deps_.store->putMessage(session_id_, assistant_);
        deps_.emitter->emitPart(MaiEventType::MessagePartUpdated, session_id_, assistant_.id,
                                part.id);

        MaiToolResult result;
        if (!tool) {
            // 模型有时会编一个不存在的工具名。告诉它事实，它下一圈通常会改对；
            // 直接失败整轮反而更糟。
            result =
                MaiToolResult::failure(MaiErrorCode::NotFound, "没有名为 " + call.name + " 的工具");
        } else if (ctx.root.empty()) {
            result = MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                            "这个会话没有设置工作目录，文件类工具无法使用");
        } else {
            bool allowed = true;
            MaiToolResult denial = checkPermission(call, part.id, allowed, cancel);
            if (!allowed) {
                result = std::move(denial);
            } else {
                if (needsApproval) {
                    // 批了才真正开跑。这条状态变化界面等着用——不发的话工具卡
                    // 会一直停在"等待授权"，直到它跑完才跳变。
                    std::get<MaiToolPart>(assistant_.parts.back().body).state =
                        MaiToolState::Running;
                    deps_.store->putMessage(session_id_, assistant_);
                    deps_.emitter->emitPart(MaiEventType::MessagePartUpdated, session_id_,
                                            assistant_.id, part.id);
                }
                result = tool->execute(call.arguments, ctx);
            }
        }

        auto& stored = std::get<MaiToolPart>(assistant_.parts.back().body);
        if (result.hasError()) {
            stored.state = MaiToolState::Error;
            stored.error = result.error().message();
            // 错误也要回灌给模型——它需要知道失败了才能换个做法。
            stored.output = result.error().message();
        } else {
            stored.state = MaiToolState::Completed;
            stored.output = result.output();
            if (result.isTruncated()) stored.output += "\n（输出已截断）";
        }
        deps_.emitter->emitPart(MaiEventType::MessagePartUpdated, session_id_, assistant_.id,
                                part.id);

        // 每执行完一个工具就落一次库：工具可能跑很久，中途崩了不该丢掉已完成的部分。
        deps_.store->putMessage(session_id_, assistant_);
    }
}

MaiToolResult MaiTurnRunner::checkPermission(const MaiToolInvocation& call,
                                             const std::string& partId, bool& allowed,
                                             const std::atomic<bool>& cancel) {
    allowed = true;
    MaiTool* tool = deps_.tools ? deps_.tools->find(call.name) : nullptr;
    if (!tool || !tool->requiresApproval()) return {};

    // 模型被拒之后经常原样再试一次。第二次不再弹框，直接回同样的话。
    const std::string signature = call.name + std::string(1, '\0') + call.arguments;
    if (rejected_.count(signature) > 0) {
        allowed = false;
        return MaiToolResult::failure(MaiErrorCode::Canceled, kRejectedHint);
    }

    if (!deps_.permissions) {
        // 没装闸门 = 没有任何人能点头。兜底成拒绝而不是放行：
        // 放行意味着"忘了接权限"这个疏忽会静默地让模型改用户的文件。
        allowed = false;
        rejected_.insert(signature);
        return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                      "这个工具需要用户授权，但当前没有接入权限闸门。");
    }

    // 用户之前对这个会话说过"以后都允许"。
    if (deps_.permissions->isAllowedInSession(session_id_, call.name)) return {};

    MaiPermissionRequest request;
    request.id = MaiIdGenerator::newPermissionId();
    request.sessionId = session_id_;
    request.messageId = assistant_.id;
    request.partId = partId;
    request.toolName = call.name;
    request.arguments = call.arguments;
    request.asked = MaiTime::getCurrentTime();

    MaiEventEmitter* emitter = deps_.emitter;
    // ask() 会阻塞这个线程直到有人裁决。每一轮跑在自己的线程上，
    // 所以这里卡住不影响别的会话（见 MaiPermission.h 的线程契约）。
    const MaiPermissionDecision decision = deps_.permissions->ask(
        request,
        [emitter](const MaiPermissionRequest& r) {
            if (emitter) emitter->emitPermissionAsked(r);
        },
        cancel);

    if (emitter) emitter->emitPermissionReplied(request, decision);

    if (decision == MaiPermissionDecision::Reject) {
        allowed = false;
        rejected_.insert(signature);
        // 中断和拒绝在闸门那边长得一样（都是 Reject），但对模型说的话要分开：
        // 中断时整轮马上就结束了，那句"换个做法"没人会读到，只会被存进历史
        // 误导下一轮。
        if (cancel.load(std::memory_order_relaxed))
            return MaiToolResult::failure(MaiErrorCode::Canceled, "已中断。");
        return MaiToolResult::failure(MaiErrorCode::Canceled, kRejectedHint);
    }
    return {};
}

void MaiTurnRunner::commitStreamedParts() {
    // 落库只在这里做。每个 delta 落一次盘等于每秒几十次 fsync。
    if (!reasoning_.empty()) {
        MaiMessagePart p;
        p.id = reasoning_part_id_;
        p.body = MaiReasoningPart{reasoning_};
        p.created = MaiTime::getCurrentTime();
        assistant_.parts.push_back(std::move(p));
        reasoning_.clear();
    }
    if (!text_.empty()) {
        MaiMessagePart p;
        p.id = text_part_id_;
        p.body = MaiTextPart{text_};
        p.created = MaiTime::getCurrentTime();
        assistant_.parts.push_back(std::move(p));
        text_.clear();
    }
}

void MaiTurnRunner::finish(const std::atomic<bool>& cancel) {
    commitStreamedParts();  // 中断时可能还有没落库的半截内容

    assistant_.completed = MaiTime::getCurrentTime();
    deps_.store->putMessage(session_id_, assistant_);
    deps_.emitter->emitMessage(MaiEventType::MessageUpdated, session_id_, assistant_.id);

    // 给会话起名不是"跑一轮"的职责，委托出去（见 session_titler.h）。
    if (deps_.titler) {
        const std::string title = deps_.titler->apply(*deps_.store, session_id_);
        if (!title.empty()) {
            deps_.emitter->emitSession(MaiEventType::SessionUpdated, session_id_, title);
        }
    }

    // 主动中断不是故障：界面不该弹错误，已经吐出来的内容也照常保留。
    const bool canceled =
        cancel.load(std::memory_order_relaxed) || error_.code() == MaiErrorCode::Canceled;
    if (error_ && !canceled) {
        deps_.emitter->emitSession(MaiEventType::SessionError, session_id_, error_.message());
    }
}
