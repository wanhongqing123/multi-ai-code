#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "MaiModelClient.h"
#include "MaiMessage.h"

// 把历史消息组装成给模型看的上下文。
//
// 单独抽出来，是因为这里将来会长出一堆策略，而它们都不该塞进 MaiAgent：
//   - 增量缓存：现在每轮 O(n) 重建整个历史，聊长了整体就是 O(n²)。
//     要改成保留已序列化的前缀、每轮只追加——这是计划里的一号性能风险。
//   - 上下文压缩：目前用字节预算裁剪旧消息、缩短巨大工具输出；
//     将来可以把被裁剪的前文摘要掉，并升级为按 token 预算估算。
//   - 项目规则文件等随历史演进的上下文注入。稳定的基础指令属于 MaiModelRequest。
// 混在 MaiAgent 里就没地方放这些，也没法单独测。
class MaiContextBuilder {
public:
    struct Options {
        // 默认不回灌历史思考草稿，避免 Chat 与 Responses 的上下文出现差异。
        // 显式启用时才把草稿交给模型。
        bool includeReasoning = false;
        // Keep the latest tool images from the active user turn. Older pixels remain on disk and
        // in the conversation store, but repeatedly embedding all of them makes every subsequent
        // model request grow until its streaming deadline is exhausted.
        std::size_t maxRecentToolImages = 8;
        // Keep only recent user image references in model text. The active user's attachments
        // are always included, so a newly sent image is never hidden by an older gallery.
        std::size_t maxRecentUserImageReferences = 12;
        // 模型只看近期上下文；完整历史仍在会话库。大型 ffprobe/工具结果仅给有界摘录，
        // 避免每轮重发兆字节级文本，让服务端在开始流式回答前就超时。
        std::size_t maxHistoryTextBytes = 256 * 1024;
        std::size_t maxToolResultBytes = 16 * 1024;
        std::size_t maxOlderMessageBytes = 32 * 1024;
        std::size_t maxReasoningBytes = 8 * 1024;
    };

    MaiContextBuilder();
    explicit MaiContextBuilder(Options options);

    std::vector<MaiModelMessage> build(const std::vector<MaiMessage>& history) const;

private:
    Options mOptions;
};
