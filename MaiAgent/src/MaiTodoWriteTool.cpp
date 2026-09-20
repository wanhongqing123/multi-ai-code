#include <json.hpp>

#include <string>

#include "MaiTodoWriteTool.h"

namespace {

using json = nlohmann::json;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

// ── todowrite ───────────────────────────────────────────────────
//
// 模型自己记的任务清单。长任务里它很容易跑偏或漏掉一步，写下来就不容易丢。
//
// **这个工具是无状态的。** 清单不存在任何地方——模型每次把**完整的**清单传进来，
// 工具只负责检查一遍再原样摆回去。清单本身活在对话历史里（就是这次调用的参数）。
// 形状和 codex 的 update_plan、opencode 的 todowrite 一致。
//
// 为什么不存：存了就要考虑存哪儿、会话删了怎么办、多端同时改怎么办，
// 而收益只是省掉模型重发一遍清单——它本来就要重发，因为它得告诉你哪一项变了。
class TodoWriteTool final : public MaiTool {
public:
    std::string name() const override {
        return "todowrite";
    }

    std::string description() const override {
        return "Record the task list for the work you are doing, and update it as you go. Send "
               "the whole list every time, not just the part that changed. Use it for anything "
               "that takes more than a couple of steps, so nothing gets dropped. Exactly one item "
               "may be in_progress at a time.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("todos":{"type":"array","description":"The whole task list, in order",)"
               R"("items":{"type":"object","properties":{)"
               R"("content":{"type":"string","description":"What the step is"},)"
               R"("status":{"type":"string","enum":["pending","in_progress","completed"]}},)"
               R"("required":["content","status"]}}},)"
               R"("required":["todos"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        (void)context;
        const json args = parseArguments(raw);
        if (!args.contains("todos") || !args["todos"].is_array()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "todos must be an array of steps");
        }

        const json& todos = args["todos"];
        int running = 0;
        int done = 0;
        std::string listing;
        for (const auto& entry : todos) {
            if (!entry.is_object()) {
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "each item in todos must be an object with content "
                                              "and status");
            }
            const std::string content = entry.value("content", std::string{});
            const std::string status = entry.value("status", std::string{});
            if (content.empty()) {
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "every item needs a non-empty content");
            }
            const char* mark = nullptr;
            if (status == "pending") {
                mark = "[ ]";
            } else if (status == "in_progress") {
                mark = "[~]";
                ++running;
            } else if (status == "completed") {
                mark = "[x]";
                ++done;
            } else {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "status must be pending, in_progress or completed, but got: " + status);
            }
            listing += mark;
            listing += ' ';
            listing += content;
            listing += '\n';
        }

        if (todos.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "todos is empty");
        }
        if (running > 1) {
            // 同时干好几件事正是跑偏的开始。**说清楚为什么**，它下一圈才改得对。
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "only one item may be in_progress at a time, but " + std::to_string(running) +
                    " are. Finish one before starting the next.");
        }

        return MaiToolResult::success(
            "Task list updated (" + std::to_string(done) + " of " +
            std::to_string(static_cast<int>(todos.size())) + " done):\n" + listing);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiTodoWriteTool() {
    return std::make_unique<TodoWriteTool>();
}
