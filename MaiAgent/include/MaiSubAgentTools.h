#pragma once

#include <memory>

#include "MaiTool.h"

// 子 Agent 那一组。名字照 codex 的 multi_agents，模型见过这套。
//
// 五个放一个文件是有意的：它们是**一个功能**，分五个头只会让人以为
// 可以单独挑着用（挑着用没有意义——起了不能等，或者能等不能收，都说不通）。
std::unique_ptr<MaiTool> makeMaiSpawnAgentTool();
std::unique_ptr<MaiTool> makeMaiWaitAgentTool();
std::unique_ptr<MaiTool> makeMaiSendInputTool();
std::unique_ptr<MaiTool> makeMaiListAgentsTool();
std::unique_ptr<MaiTool> makeMaiCloseAgentTool();
