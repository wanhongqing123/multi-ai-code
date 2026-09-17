#include "MaiContextBuilder.h"

MaiContextBuilder::MaiContextBuilder(Options options) : options_(std::move(options)) {}

std::vector<MaiModelMessage> MaiContextBuilder::build(
    const std::vector<MaiMessage>& history) const {
    std::vector<MaiModelMessage> out;
    out.reserve(history.size() + 1);

    if (!options_.systemPrompt.empty()) {
        MaiModelMessage t;
        t.role = MaiModelRole::System;
        t.content = options_.systemPrompt;
        out.push_back(std::move(t));
    }

    for (const auto& m : history) {
        if (m.role == MaiRole::User) {
            MaiModelMessage t;
            t.role = MaiModelRole::User;
            t.content = m.text();
            if (!t.content.empty()) out.push_back(std::move(t));
            continue;
        }

        // ── assistant 这一侧要按工具调用切段 ────────────────────────
        //
        // 一条 assistant 消息里可能是：说几句 → 调工具 → 拿到结果 → 再说几句。
        // 而 OpenAI 协议要求的形状是：
        //   assistant（带 tool_calls）
        //   tool（结果，一个调用一条）
        //   assistant（后续文本）
        // 所以不能把整条消息压成一个 turn——那样模型看不到调用和结果的对应关系，
        // 下一轮会重复调同一个工具。
        MaiModelMessage pending;
        pending.role = MaiModelRole::Assistant;
        std::vector<const MaiToolPart*> batch;

        auto flush_batch = [&] {
            if (batch.empty()) return;
            // 先发 assistant + 它发起的这批调用
            for (const auto* tp : batch) {
                MaiToolInvocation inv;
                inv.id = tp->callId;
                inv.name = tp->tool;
                inv.arguments = tp->input;
                pending.invocations.push_back(std::move(inv));
            }
            out.push_back(pending);
            pending = MaiModelMessage{};
            pending.role = MaiModelRole::Assistant;

            // 再发每个调用的结果。toolCallId 必须对得上，否则模型认不出
            // 这是哪次调用的结果。
            for (const auto* tp : batch) {
                MaiModelMessage r;
                r.role = MaiModelRole::ToolResult;
                r.toolCallId = tp->callId;
                r.content = tp->output.empty() ? "（无输出）" : tp->output;
                out.push_back(std::move(r));
            }
            batch.clear();
        };

        for (const auto& p : m.parts) {
            if (const auto* text = std::get_if<MaiTextPart>(&p.body)) {
                // 工具调用之后又开口说话了，说明上一批已经结束，先结算。
                flush_batch();
                if (!pending.content.empty()) pending.content += "\n";
                pending.content += text->text;
            } else if (const auto* r = std::get_if<MaiReasoningPart>(&p.body)) {
                // 默认不回灌。reasoning 是模型的草稿，喂回去会污染下一轮的判断。
                if (options_.includeReasoning) {
                    flush_batch();
                    if (!pending.content.empty()) pending.content += "\n";
                    pending.content += r->text;
                }
            } else if (const auto* tp = std::get_if<MaiToolPart>(&p.body)) {
                // 还没跑完的不回灌：模型看到一个没有结果的调用会以为它失败了。
                if (tp->state == MaiToolState::Completed || tp->state == MaiToolState::Error) {
                    batch.push_back(tp);
                }
            }
        }
        flush_batch();

        if (!pending.content.empty() || !pending.invocations.empty()) {
            out.push_back(std::move(pending));
        }
    }
    return out;
}
