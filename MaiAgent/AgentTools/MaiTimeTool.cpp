#include "MaiTimeTool.h"

#include <string>

#include "MaiTime.h"

namespace {

// 模型不知道今天几号。不给它的话它会按训练时的日期去推算「最近」「三天前」，
// 而那个日期可能是一年前。形状照 codex 的 current_time。
class CurrentTimeTool final : public MaiTool {
public:
    std::string name() const override {
        return "current_time";
    }

    std::string description() const override {
        return "Return the current date and time. Use it before reasoning about anything relative "
               "to now, such as how recent a commit or a log entry is.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{}})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        (void)raw;
        (void)context;
        return MaiToolResult::success(MaiTime::formatIso8601Utc(MaiTime::getCurrentTime()));
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCurrentTimeTool() {
    return std::make_unique<CurrentTimeTool>();
}
