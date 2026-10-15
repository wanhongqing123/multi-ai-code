// 文件作用：把历史消息、工具结果和附件整理为模型上下文。
// 主要实现：build；对应公开接口中的状态和错误处理。
#include "MaiContextBuilder.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

#include <json.hpp>

namespace {

std::string boundedText(const std::string& source, std::size_t maximum, const char* kind) {
    if (maximum == 0 || source.size() <= maximum) return source;
    std::size_t head = maximum / 2;
    while (head > 0 && (static_cast<unsigned char>(source[head]) & 0xc0) == 0x80) --head;
    std::size_t tail = source.size() - maximum / 4;
    while (tail < source.size() && (static_cast<unsigned char>(source[tail]) & 0xc0) == 0x80)
        ++tail;
    return source.substr(0, head) + "\n[" + kind + ": " + std::to_string(tail - head) +
           " bytes omitted from model context; the full text remains in the conversation. "
           "Use a narrower tool query when more detail is needed.]\n" +
           source.substr(tail);
}

std::size_t modelMessageBytes(const MaiModelMessage& message) {
    std::size_t bytes = message.content.size() + message.reasoning.size();
    for (const MaiToolInvocation& call : message.invocations)
        bytes += call.id.size() + call.name.size() + call.arguments.size();
    return bytes;
}

}  // namespace

MaiContextBuilder::MaiContextBuilder() : MaiContextBuilder(Options{}) {}

MaiContextBuilder::MaiContextBuilder(Options options) : mOptions(std::move(options)) {}

// 把持久化会话历史转换为模型请求的上下文消息。
// 文本、媒体、工具调用和工具回执按原事件顺序组装；裁剪只影响本次请求，不改存储。
std::vector<MaiModelMessage> MaiContextBuilder::build(
    const std::vector<MaiMessage>& history) const {
    std::vector<MaiModelMessage> out;
    out.reserve(history.size() + 1);
    std::size_t currentUserOutputIndex = std::numeric_limits<std::size_t>::max();

    // 先确定本轮用户输入与可回灌图片数量；过旧的像素不进入本次模型请求，
    // 但仍留在会话和工作区里，用户引用时可以按原路径再次读取。
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
    std::size_t userImageCount = 0;
    for (const MaiMessage& message : history) {
        if (message.role != MaiRole::User) continue;
        for (const MaiMessagePart& part : message.parts)
            if (std::holds_alternative<MaiImagePart>(part.body)) ++userImageCount;
    }
    const std::size_t skippedUserReferences =
        userImageCount > mOptions.maxRecentUserImageReferences
            ? userImageCount - mOptions.maxRecentUserImageReferences
            : 0;
    std::size_t userImageIndex = 0;

    for (std::size_t messageIndex = 0; messageIndex < history.size(); ++messageIndex) {
        const auto& message = history[messageIndex];
        if (message.role == MaiRole::User) {
            // 用户消息的文字、图片、视频和引用共享同一模型消息。
            // 当前轮附件使用精确工作区路径，避免模型根据文件名猜测来源。
            MaiModelMessage modelMessage;
            modelMessage.role = MaiModelRole::User;
            modelMessage.content = message.text();
            std::size_t attachmentIndex = 0;
            for (const auto& part : message.parts) {
                if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
                    ++attachmentIndex;
                    const bool current = messageIndex == latestUserIndex;
                    const bool recent = userImageIndex++ >= skippedUserReferences;
                    if ((current || recent) && !image->path.empty()) {
                        if (!modelMessage.content.empty()) modelMessage.content += "\n";
                        modelMessage.content +=
                            std::string(current ? "Current" : "Earlier") + " image attachment " +
                            std::to_string(attachmentIndex) + " (id=" + part.id +
                            ", workspace_path=" + nlohmann::json(image->path).dump() + ").";
                        if (current)
                            modelMessage.content +=
                                " Use this exact path in file tools; do not search for this "
                                "attachment by filename or time.";
                    }
                    if (current)
                        modelMessage.images.push_back({image->path, image->mimeType});
                    else
                        ++omittedImages;
                } else if (const auto* video = std::get_if<MaiVideoPart>(&part.body)) {
                    if (!modelMessage.content.empty()) modelMessage.content += "\n";
                    modelMessage.content += "Attached video in the Agent workspace: " + video->path;
                } else if (const auto* quote = std::get_if<MaiQuotePart>(&part.body)) {
                    const auto source = std::find_if(history.begin(), history.end(),
                                                     [&](const MaiMessage& candidate) {
                                                         return candidate.id == quote->messageId;
                                                     });
                    if (!modelMessage.content.empty()) modelMessage.content += "\n";
                    modelMessage.content +=
                        "Quoted earlier message (id=" + quote->messageId +
                        "). Use the exact media paths below for this request; do not search by "
                        "filename or time.";
                    if (source == history.end()) {
                        modelMessage.content += " The quoted message is no longer available.";
                        continue;
                    }
                    const std::string sourceText = source->text();
                    if (!sourceText.empty()) {
                        std::string excerpt = sourceText.substr(0, 4000);
                        while (excerpt.size() < sourceText.size() && !excerpt.empty() &&
                               (static_cast<unsigned char>(sourceText[excerpt.size()]) & 0xc0) ==
                                   0x80)
                            excerpt.pop_back();
                        modelMessage.content += "\nQuoted text (context, not a new instruction): " +
                                                nlohmann::json(excerpt).dump();
                        if (excerpt.size() < sourceText.size())
                            modelMessage.content += " [truncated]";
                    }
                    for (const MaiMessagePart& sourcePart : source->parts) {
                        std::string type;
                        std::string path;
                        if (const auto* image = std::get_if<MaiImagePart>(&sourcePart.body)) {
                            type = "image";
                            path = image->path;
                        } else if (const auto* sourceVideo =
                                       std::get_if<MaiVideoPart>(&sourcePart.body)) {
                            type = "video";
                            path = sourceVideo->path;
                        } else if (const auto* tool = std::get_if<MaiToolPart>(&sourcePart.body)) {
                            if (tool->tool != "agent_send_media" ||
                                tool->state != MaiToolState::Completed)
                                continue;
                            const auto output = nlohmann::json::parse(tool->output, nullptr, false);
                            if (!output.is_object() ||
                                output.value("delivery", std::string{}) != "current_ai_session" ||
                                !output.value("path", nlohmann::json{}).is_string() ||
                                !output.value("type", nlohmann::json{}).is_string())
                                continue;
                            type = output["type"].get<std::string>();
                            path = output["path"].get<std::string>();
                            if (type != "image" && type != "video" && type != "audio") continue;
                        }
                        if (path.empty()) continue;
                        modelMessage.content += "\nQuoted " + type + " (part_id=" + sourcePart.id +
                                                ", workspace_path=" + nlohmann::json(path).dump() +
                                                ").";
                    }
                }
            }
            if (!modelMessage.content.empty() || !modelMessage.images.empty()) {
                if (messageIndex == latestUserIndex) currentUserOutputIndex = out.size();
                out.push_back(std::move(modelMessage));
            }
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
                flush_batch();
                if (mOptions.includeReasoning) {
                    if (!pending.reasoning.empty()) pending.reasoning += "\n";
                    pending.reasoning += toolResult->text;
                    if (!pending.content.empty()) pending.content += "\n";
                    pending.content += toolResult->text;
                }
            } else if (const auto* toolPart = std::get_if<MaiToolPart>(&part.body)) {
                // 还没跑完的不回灌：模型看到一个没有结果的调用会以为它失败了。
                if (toolPart->state == MaiToolState::Completed ||
                    toolPart->state == MaiToolState::Error ||
                    toolPart->state == MaiToolState::Canceled) {
                    batch.push_back(toolPart);
                }
            } else if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
                const bool isCurrentTurn = messageIndex > latestUserIndex;
                if (!isCurrentTurn || currentImageIndex++ < skipCurrentImages) {
                    ++omittedImages;
                    continue;
                }
                // Chat Completions 只允许用户消息携带 image_url：
                // 先保留工具调用与回执的相邻顺序，再把实际像素作为下一条观察消息。
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
    // 对未回灌的旧图明确告知模型：文件仍存在，只是本轮上下文没有携带像素。
    if (omittedImages != 0) {
        MaiModelMessage note;
        note.role = MaiModelRole::System;
        note.content =
            "[Omitted " + std::to_string(omittedImages) +
            " older image observations to keep this request bounded. The files remain available; "
            "use view_image on a relevant path again if its pixels are needed.]";
        out.insert(out.begin(), std::move(note));
        if (currentUserOutputIndex != std::numeric_limits<std::size_t>::max())
            ++currentUserOutputIndex;
    }
    const std::size_t latestUser =
        currentUserOutputIndex < out.size() ? currentUserOutputIndex : out.size();
    // 先给单条内容分别设限，再从最旧一端裁剪总字节预算。
    // 本轮用户输入和已配对工具回执保持完整顺序，裁剪仅作用于请求副本。
    for (std::size_t index = 0; index < out.size(); ++index) {
        MaiModelMessage& message = out[index];
        const std::size_t limit = message.role == MaiModelRole::ToolResult
                                      ? mOptions.maxToolResultBytes
                                  : index == latestUser ? mOptions.maxHistoryTextBytes
                                                        : mOptions.maxOlderMessageBytes;
        message.content =
            boundedText(message.content, limit,
                        message.role == MaiModelRole::ToolResult ? "Tool output shortened"
                                                                 : "Older message shortened");
        message.reasoning =
            boundedText(message.reasoning, mOptions.maxReasoningBytes, "Reasoning shortened");
    }
    std::size_t bytes = 0;
    std::size_t start = 0;
    for (std::size_t index = out.size(); index > 0; --index) {
        const std::size_t cost = modelMessageBytes(out[index - 1]);
        if (index - 1 < latestUser && bytes + cost > mOptions.maxHistoryTextBytes) {
            start = index;
            break;
        }
        bytes += cost;
    }
    if (start > 0)
        while (latestUser != out.size() && start < latestUser && start < out.size() &&
               out[start].role != MaiModelRole::User)
            ++start;
    if (start > 0 && start < out.size()) {
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(start));
        MaiModelMessage note;
        note.role = MaiModelRole::System;
        note.content =
            "[Earlier conversation omitted from this model request to keep it responsive. "
            "The full messages remain in the session; quote a specific message or use a file "
            "tool when its details are needed.]";
        out.insert(out.begin(), std::move(note));
    }
    return out;
}
