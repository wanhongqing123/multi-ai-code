#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiMessage.h"
#include "MaiSession.h"

// 持久化接口。
//
// 和领域模型分开：MaiSession / MaiMessage 是纯数据，它们不该知道
// 持久化的存在。反过来放在一个头里，任何人 include 领域模型就把
// 存储接口也拖了进来。
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

    // 原子地改会话的一部分字段。
    //
    // 存在的理由是个真 bug：之前跑一轮的流程是"开头读会话、结尾改标题写回"，
    // 中间如果别人改了 model 或 agent，收尾那一写会把人家的改动盖掉
    // （lost update）。让存储层在锁内做读-改-写，调用方就碰不到这个窗口。
    // 返回 false 表示会话不存在。
    virtual bool mutateSession(const std::string& id,
                               const std::function<void(MaiSession&)>& mutator) = 0;
};

std::unique_ptr<MaiSessionStore> makeMaiMemoryStore();
