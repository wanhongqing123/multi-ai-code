#include "MaiWindowListTool.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <json.hpp>

#include "MaiScreenshot.h"

using json = nlohmann::json;

namespace {

constexpr std::size_t kMaximumWindowResults = 100;

class MaiWindowListTool final : public MaiTool {
public:
    std::string name() const override {
        return "list_windows";
    }

    std::string description() const override {
        return "List visible top-level windows that can be passed to screenshot mode=window. "
               "Use this when the user names an application but its exact window title is unknown.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"required":[],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext&) override {
        const json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
        if (arguments.is_discarded() || !arguments.is_object() || !arguments.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "list_windows expects an empty JSON object");
        }

        std::vector<std::string> titles;
        const MaiError error = maiListCaptureWindows(titles);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());
        if (titles.empty()) return MaiToolResult::success("No capturable windows are visible.");

        const bool truncated = titles.size() > kMaximumWindowResults;
        if (truncated) titles.resize(kMaximumWindowResults);
        std::string output;
        for (const std::string& title : titles) {
            output += title;
            output += '\n';
        }
        if (truncated) output += "...Window list truncated; use a title from the visible results.";
        return MaiToolResult::success(std::move(output), truncated);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiWindowListTool() {
    return std::make_unique<MaiWindowListTool>();
}
