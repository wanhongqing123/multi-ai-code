// 文件作用：把历史消息、工具结果和附件整理为模型上下文。
// 对外类型：MaiContextBuilder；线程、失败和生命周期以类型附近注释为准。
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
        // 只保留当前用户轮次里最新的工具图片。旧像素仍在磁盘和会话存储中，
        // 但每轮都重新嵌入全部图片会让模型请求不断膨胀，最终耗尽流式期限。
        std::size_t maxRecentToolImages = 8;
        // 模型文本中只保留最近的用户图片引用；当前用户新发送的附件始终保留，
        // 不能被旧图集挤掉。
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
