#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiMessage.h"
#include "MaiSession.h"

// 持久化接口。
//
// 和领域模型分开：MaiSession / MaiMessage 是纯数据，它们不该知道持久化的存在。反过来放在一个头里，
// 任何人 include 领域模型就把存储接口也拖了进来。
//
// 第一天就抽接口的另一个原因：嵌入式上可能根本不落盘，或者换成别的 KV。
class MaiSessionStore {
public:
    virtual ~MaiSessionStore() = default;

    virtual void putSession(const MaiSession& session) = 0;
    virtual bool getSession(const std::string& id, MaiSession& out) const = 0;
    virtual std::vector<MaiSession> listSessions() const = 0;  // 按 updated 倒序
    virtual bool removeSession(const std::string& id) = 0;

    virtual void putMessage(const std::string& sessionId, const MaiMessage& message) = 0;
    virtual std::vector<MaiMessage> listMessages(const std::string& sessionId) const = 0;

    // 最近一次写入失败了吗？没失败返回一个空的 MaiError。
    //
    // 为什么写接口返回 void、错误另开一个查询：putMessage 在流式的热路径上被反复调用，
    // 每个调用点都 if 一下会把代码淹掉，而且那些地方也做不了什么补救。
    // 但**失败不能无声无息**——磁盘满了还假装存上了，是用户第二天打开发现对话没了的那种 bug。
    //
    // 约定：一轮结束时查一次（MaiTurnRunner::finish），失败就把会话标成
    // 错误让界面看见。
    virtual MaiError lastWriteError() const = 0;

    // 原子地改会话的一部分字段。
    //
    // 存在的理由是个真 bug：之前跑一轮的流程是"开头读会话、结尾改标题写回"，
    // 中间如果别人改了 model 或 agent，收尾那一写会把人家的改动盖掉（lost update）。
    // 让存储层在锁内做读-改-写，调用方就碰不到这个窗口。返回 false 表示会话不存在。
    virtual bool mutateSession(const std::string& id,
                               const std::function<void(MaiSession&)>& mutator) = 0;
};

std::unique_ptr<MaiSessionStore> makeMaiMemoryStore();

// SQLite 落库。databasePathUtf8 是 **UTF-8** 路径；父目录会自动建。
//
// 传 ":memory:" 可以拿到一个不落盘的 SQLite 库——那和 makeMaiMemoryStore()不是一回事：
// 后者是纯 STL 的哈希表，前者仍然走完整的 SQL 路径，测试里用它来验证 SQL 本身，而不用碰磁盘。
//
// 打不开（路径非法、磁盘只读、文件不是数据库）就返回错误，不返回一个半死的对象。
// 这是唯一一处把存储错误做成返回值的地方，因为构造失败时调用方**能**做点什么：换个路径，
// 或者退回内存存储。
MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(const std::string& databasePathUtf8);
