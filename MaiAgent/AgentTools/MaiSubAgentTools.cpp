#include <json.hpp>

#include <string>

#include "MaiSubAgent.h"
#include "MaiSubAgentTools.h"

// 子 Agent 的五个工具：spawn / send / wait / list / close。
//
// 名字和形状照 codex 的 multi_agents（spawn_agent / send_input / wait_agent /
// list_agents / close_agent），不自己发明——模型见过那套。
//
// ── 为什么是「起了不等」而不是「起了就等」 ────────────────────
//
// spawn_agent 立刻返回 id，父接着干自己的活；要结论的时候再 wait_agent。
// 合成一个「跑一个子任务并等它」的工具会省掉一次调用，但那样就没法同时起三个了，
// 而**并行正是这套东西为数不多的收益之一**。

namespace {

using json = nlohmann::json;

constexpr MaiMillis kDefaultWaitMs = 120000;
constexpr MaiMillis kMaxWaitMs = 600000;
// 回给父的结论有多长。子 Agent 的整段回答可能很长，而父要的是结论——
// 真要全文的话，父可以自己去读那个会话。
constexpr std::size_t kMaxReportBytes = 8 * 1024;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

// 五个工具都要先确认宿主接了子 Agent。没接就明说，别让模型反复试。
MaiToolResult requireHost(const MaiToolContext& context) {
    if (context.subAgents != nullptr) return {};
    return MaiToolResult::failure(
        MaiErrorCode::NotSupported,
        "this app does not support sub-agents. Do the work yourself in this conversation.");
}

std::string clip(std::string text) {
    if (text.size() <= kMaxReportBytes) return text;
    text.resize(kMaxReportBytes);
    text += "\n[the sub-agent said more than this; read its session if you need the rest]";
    return text;
}

// ── spawn_agent ─────────────────────────────────────────────────
class SpawnAgentTool final : public MaiTool {
public:
    std::string name() const override {
        return "spawn_agent";
    }

    std::string description() const override {
        return "Start a sub-agent to do one self-contained piece of work, and keep going "
               "yourself. It returns an id immediately; use wait_agent when you need its answer. "
               "The point is to keep a large search out of your own context: let it read thirty "
               "files and tell you the one conclusion. It works in the same directory as you and "
               "has to ask the user for its own approvals. Do not use it for work you could "
               "finish in a couple of steps yourself.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("task_name":{"type":"string","description":"A short name for the task, used when reporting status"},)"
               R"("prompt":{"type":"string","description":"The whole task. The sub-agent cannot see this conversation, so say everything it needs."}},)"
               R"("required":["task_name","prompt"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const MaiToolResult missing = requireHost(context);
        if (missing.hasError()) return missing;

        const json args = parseArguments(raw);
        const std::string taskName = args.value("task_name", std::string{});
        const std::string prompt = args.value("prompt", std::string{});
        if (prompt.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: prompt");
        }

        MaiResult<std::string> spawned =
            context.subAgents->spawnSubAgent(context.sessionId, taskName, prompt);
        if (!spawned) {
            return MaiToolResult::failure(spawned.error().code(), spawned.error().message());
        }
        return MaiToolResult::success("Started sub-agent " + spawned.value() +
                                      " for \"" + taskName +
                                      "\". Use wait_agent with that id when you need its answer.");
    }
};

// ── wait_agent ──────────────────────────────────────────────────
class WaitAgentTool final : public MaiTool {
public:
    std::string name() const override {
        return "wait_agent";
    }

    std::string description() const override {
        return "Wait for a sub-agent to finish what it is doing and return what it said. If it "
               "is still working when the timeout runs out, you get told that instead, and you "
               "can wait again.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("agent_id":{"type":"string","description":"The id spawn_agent gave you"},)"
               R"("timeout_ms":{"type":"integer","description":"Give up waiting after this long, default 120000, max 600000"}},)"
               R"("required":["agent_id"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const MaiToolResult missing = requireHost(context);
        if (missing.hasError()) return missing;

        const json args = parseArguments(raw);
        const std::string childId = args.value("agent_id", std::string{});
        if (childId.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: agent_id");
        }
        MaiMillis timeout = args.value("timeout_ms", kDefaultWaitMs);
        timeout = timeout < 1000 ? 1000 : (timeout > kMaxWaitMs ? kMaxWaitMs : timeout);

        static const std::atomic<bool> kNeverCanceled{false};
        const std::atomic<bool>& cancel =
            context.cancel != nullptr ? *context.cancel : kNeverCanceled;
        const bool finished =
            context.subAgents->waitForSubAgent(context.sessionId, childId, timeout, cancel);

        if (!finished) {
            if (cancel.load(std::memory_order_relaxed)) {
                return MaiToolResult::failure(MaiErrorCode::Canceled,
                                              "the turn was interrupted while waiting");
            }
            // **说清楚是还没跑完，不是失败了。** 说成失败的话模型会去收拾一个
            // 根本没出错的子 Agent。
            return MaiToolResult::success("Sub-agent " + childId +
                                          " is still working. Wait again, or do something else "
                                          "and come back to it.");
        }

        const std::string report = context.subAgents->subAgentReport(childId);
        if (report.empty()) {
            return MaiToolResult::success("Sub-agent " + childId +
                                          " finished without saying anything.");
        }
        return MaiToolResult::success(clip(report));
    }
};

// ── send_input ──────────────────────────────────────────────────
class SendInputTool final : public MaiTool {
public:
    std::string name() const override {
        return "send_input";
    }

    std::string description() const override {
        return "Give a sub-agent you already started another instruction, for example to answer "
               "a question it raised or to redirect it. Reuse a sub-agent this way when the new "
               "work depends on what it already learned; start a fresh one when it does not.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("agent_id":{"type":"string","description":"The id spawn_agent gave you"},)"
               R"("prompt":{"type":"string","description":"What to tell it"}},)"
               R"("required":["agent_id","prompt"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const MaiToolResult missing = requireHost(context);
        if (missing.hasError()) return missing;

        const json args = parseArguments(raw);
        const std::string childId = args.value("agent_id", std::string{});
        const std::string prompt = args.value("prompt", std::string{});
        if (childId.empty() || prompt.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "agent_id and prompt are both required");
        }
        const MaiError failure =
            context.subAgents->sendToSubAgent(context.sessionId, childId, prompt);
        if (failure.hasError()) {
            return MaiToolResult::failure(failure.code(), failure.message());
        }
        return MaiToolResult::success("Sent. Use wait_agent to get the answer.");
    }
};

// ── list_agents ─────────────────────────────────────────────────
class ListAgentsTool final : public MaiTool {
public:
    std::string name() const override {
        return "list_agents";
    }

    std::string description() const override {
        return "List the sub-agents you started and whether each one is running, idle or closed.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{}})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        (void)raw;
        const MaiToolResult missing = requireHost(context);
        if (missing.hasError()) return missing;

        const std::vector<MaiSubAgentInfo> children =
            context.subAgents->listSubAgents(context.sessionId);
        if (children.empty()) return MaiToolResult::success("You have not started any sub-agents.");

        std::string listing;
        for (const MaiSubAgentInfo& child : children) {
            listing += child.status;
            listing += '\t';
            listing += child.sessionId;
            listing += '\t';
            listing += child.taskName;
            listing += '\n';
        }
        return MaiToolResult::success(std::move(listing));
    }
};

// ── close_agent ─────────────────────────────────────────────────
class CloseAgentTool final : public MaiTool {
public:
    std::string name() const override {
        return "close_agent";
    }

    std::string description() const override {
        return "Close a sub-agent you are done with. It stops whatever it is doing and frees up "
               "a slot, so do this rather than leaving finished ones open. What it said is kept.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("agent_id":{"type":"string","description":"The id spawn_agent gave you"}},)"
               R"("required":["agent_id"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const MaiToolResult missing = requireHost(context);
        if (missing.hasError()) return missing;

        const json args = parseArguments(raw);
        const std::string childId = args.value("agent_id", std::string{});
        if (childId.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: agent_id");
        }
        const MaiError failure = context.subAgents->closeSubAgent(context.sessionId, childId);
        if (failure.hasError()) {
            return MaiToolResult::failure(failure.code(), failure.message());
        }
        return MaiToolResult::success("Closed " + childId + ".");
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiSpawnAgentTool() {
    return std::make_unique<SpawnAgentTool>();
}
std::unique_ptr<MaiTool> makeMaiWaitAgentTool() {
    return std::make_unique<WaitAgentTool>();
}
std::unique_ptr<MaiTool> makeMaiSendInputTool() {
    return std::make_unique<SendInputTool>();
}
std::unique_ptr<MaiTool> makeMaiListAgentsTool() {
    return std::make_unique<ListAgentsTool>();
}
std::unique_ptr<MaiTool> makeMaiCloseAgentTool() {
    return std::make_unique<CloseAgentTool>();
}
