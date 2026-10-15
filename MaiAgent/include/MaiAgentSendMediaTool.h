// 文件作用：把 Agent 生成的媒体文件交付给聊天或宿主界面。
// 对外类型：MaiAgentMedia；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiTool.h"

struct MaiAgentMedia {
    std::string relativePath;
    std::string type;
    std::string mimeType;
    std::uint64_t bytes = 0;
};

// 校验当前 Agent 工作区中的普通文件，供本会话长期展示。
// 输入必须相对于 context.root；符号链接越界会被拒绝。
// 本工具不向 IM 联系人发送内容，也不修改源文件。只要工作区文件仍存在，
// 返回路径在会话期间保持稳定；可以在工作线程调用，不持有单轮上下文。
MaiResult<MaiAgentMedia> maiPrepareAgentMedia(const std::string& filePath, const std::string& type,
                                              const MaiToolContext& context);

// 工具完成结果按普通 assistant 工具片段持久化。
// MaiChat 在当前创作会话中将其呈现为图片、视频或音频卡片，不涉及 IM 的 peer_id。
std::unique_ptr<MaiTool> makeMaiAgentSendMediaTool();
