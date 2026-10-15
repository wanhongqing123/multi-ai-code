// 文件作用：实现内存中的 Agent 会话和消息存储。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiSessionStore.h"

// 纯内存的存储：一组哈希表，进程一退就没了。
//
// 给测试和「不想落盘」的宿主用。和 makeMaiSqliteStore(":memory:") **不是一回事**——
// 那个仍然走完整的 SQL 路径（用来验 SQL 本身），这个连 SQLite 都不碰。
std::unique_ptr<MaiSessionStore> makeMaiMemoryStore();
