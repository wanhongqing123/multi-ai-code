#include "MaiContextBuilder.h"

#include <cstddef>
#include <string>
#include <utility>

MaiContextBuilder::MaiContextBuilder() : MaiContextBuilder(Options{}) {}

MaiContextBuilder::MaiContextBuilder(Options options) : mOptions(std::move(options)) {}

std::vector<MaiModelMessage> MaiContextBuilder::build(
    const std::vector<MaiMessage>& history) const {
    std::vector<MaiModelMessage> out;
    out.reserve(history.size());

    std::size_t latestUserIndex = history.size();
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (history[index].role == MaiRole::User) latestUserIndex = index;
    }
    std::size_t currentToolImages = 0;
    for (std::size_t index = latestUserIndex; index < history.size(); ++index) {
        if (history[index].role != MaiRole::Assistant) continue;
        for (const auto& part : history[index].parts) {
            if (std::holds_alternative<MaiImagePart>(part.body)) ++currentToolImages;
        }
    }
    const std::size_t skipCurrentImages = currentToolImages > mOptions.maxRecentToolImages
                                              ? currentToolImages - mOptions.maxRecentToolImages
                                              : 0;
    std::size_t currentImageIndex = 0;
    std::size_t omittedImages = 0;

    for (std::size_t messageIndex = 0; messageIndex < history.size(); ++messageIndex) {
        const auto& message = history[messageIndex];
        if (message.role == MaiRole::User) {
            MaiModelMessage modelMessage;
            modelMessage.role = MaiModelRole::User;
            modelMessage.content = message.text();
            for (const auto& part : message.parts) {
                if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
                    if (messageIndex == latestUserIndex)
                        modelMessage.images.push_back({image->path, image->mimeType});
                    else
                        ++omittedImages;
                }
            }
            if (!modelMessage.content.empty() || !modelMessage.images.empty())
                out.push_back(std::move(modelMessage));
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
            for (const auto* toolPart : batch) {
                MaiToolInvocation inv;
                inv.id = toolPart->callId;
                inv.name = toolPart->tool;
                inv.arguments = toolPart->input;
                pending.invocations.push_back(std::move(inv));
            }
            out.push_back(pending);
            pending = MaiModelMessage{};
            pending.role = MaiModelRole::Assistant;

            // 再发每个调用的结果。toolCallId 必须对得上，否则模型认不出这是哪次调用的结果。
            for (const auto* toolPart : batch) {
                MaiModelMessage toolResult;
                toolResult.role = MaiModelRole::ToolResult;
                toolResult.toolCallId = toolPart->callId;
                toolResult.content = toolPart->output.empty() ? "(no output)" : toolPart->output;
                out.push_back(std::move(toolResult));
            }
            batch.clear();
        };

        for (const auto& part : message.parts) {
            if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
                // 工具调用之后又开口说话了，说明上一批已经结束，先结算。
                flush_batch();
                if (!pending.content.empty()) pending.content += "\n";
                pending.content += text->text;
            } else if (const auto* toolResult = std::get_if<MaiReasoningPart>(&part.body)) {
                // 默认不回灌。reasoning 是模型的草稿，喂回去会污染下一轮的判断。
                if (mOptions.includeReasoning) {
                    flush_batch();
                    if (!pending.content.empty()) pending.content += "\n";
                    pending.content += toolResult->text;
                }
            } else if (const auto* toolPart = std::get_if<MaiToolPart>(&part.body)) {
                // 还没跑完的不回灌：模型看到一个没有结果的调用会以为它失败了。
                if (toolPart->state == MaiToolState::Completed ||
                    toolPart->state == MaiToolState::Error) {
                    batch.push_back(toolPart);
                }
            } else if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
                const bool isCurrentTurn = messageIndex > latestUserIndex;
                if (!isCurrentTurn || currentImageIndex++ < skipCurrentImages) {
                    ++omittedImages;
                    continue;
                }
                // Chat Completions only accepts image_url parts on a user message. Keep the tool
                // result immediately after its assistant call, then add the captured image as the
                // next observation so the model receives the actual pixels instead of a path.
                flush_batch();
                MaiModelMessage observation;
                observation.role = MaiModelRole::User;
                observation.content = "Image returned by the preceding tool.";
                observation.images.push_back({image->path, image->mimeType});
                out.push_back(std::move(observation));
            }
        }
        flush_batch();

        if (!pending.content.empty() || !pending.invocations.empty()) {
            out.push_back(std::move(pending));
        }
    }
    if (omittedImages != 0) {
        MaiModelMessage note;
        note.role = MaiModelRole::User;
        note.content = "Omitted " + std::to_string(omittedImages) +
                       " older image observations from this request to keep the media payload "
                       "bounded. The files remain available; use view_image on a relevant path "
                       "again if its pixels are needed.";
        out.push_back(std::move(note));
    }
    return out;
}
