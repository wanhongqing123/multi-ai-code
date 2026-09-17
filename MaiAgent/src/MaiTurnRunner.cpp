#include "MaiTurnRunner.h"

#include "MaiIdGenerator.h"

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
        body.state = MaiToolState::Running;
        part.body = body;
        assistant_.parts.push_back(part);

        // 先广播"开始跑了"，界面立刻能显示工具卡，而不是等它跑完才蹦出来。
        deps_.emitter->emitPart(MaiEventType::MessagePartUpdated, session_id_, assistant_.id,
                                part.id);

        MaiTool* tool = deps_.tools ? deps_.tools->find(call.name) : nullptr;
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
            result = tool->execute(call.arguments, ctx);
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
