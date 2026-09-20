#pragma once

#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiSessionStore.h"

// SQLite 落库。databasePathUtf8 是 **UTF-8** 路径；父目录会自动建。
//
// 传 ":memory:" 可以拿到一个不落盘的 SQLite 库——那和 makeMaiMemoryStore() 不是一回事：
// 后者是纯 STL 的哈希表，前者仍然走完整的 SQL 路径，测试里用它来验证 SQL 本身，
// 而不用碰磁盘。
//
// 打不开（路径非法、磁盘只读、文件不是数据库）就返回错误，不返回一个半死的对象。
// 这是唯一一处把存储错误做成返回值的地方，因为构造失败时调用方**能**做点什么：
// 换个路径，或者退回内存存储。
MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(const std::string& databasePathUtf8);
