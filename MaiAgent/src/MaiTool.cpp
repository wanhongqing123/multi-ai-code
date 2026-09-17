#include "MaiTool.h"

#include <algorithm>

// ── MaiToolContext ──────────────────────────────────────────────

bool MaiToolContext::isCanceled() const {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

// ── MaiToolResult ───────────────────────────────────────────────

MaiToolResult MaiToolResult::success(std::string output, bool truncated) {
    MaiToolResult result;
    result.output_ = std::move(output);
    result.truncated_ = truncated;
    return result;
}

MaiToolResult MaiToolResult::failure(MaiErrorCode code, std::string message) {
    MaiToolResult result;
    result.error_ = MaiError::make(code, std::move(message));
    return result;
}

const std::string& MaiToolResult::output() const {
    return output_;
}

const MaiError& MaiToolResult::error() const {
    return error_;
}

bool MaiToolResult::isTruncated() const {
    return truncated_;
}

bool MaiToolResult::hasError() const {
    return error_.hasError();
}

// ── MaiTool ─────────────────────────────────────────────────────

bool MaiTool::requiresApproval() const {
    return false;
}

// ── MaiToolRegistry ─────────────────────────────────────────────

void MaiToolRegistry::add(std::unique_ptr<MaiTool> tool) {
    if (!tool) return;
    const std::string name = tool->name();
    // 同名覆盖：注册表里不该出现两个同名工具，模型按名字调，撞了就不确定跑哪个。
    auto existing = std::find_if(
        tools_.begin(), tools_.end(),
        [&](const std::unique_ptr<MaiTool>& candidate) { return candidate->name() == name; });
    if (existing != tools_.end()) {
        *existing = std::move(tool);
        return;
    }
    tools_.push_back(std::move(tool));
}

MaiTool* MaiToolRegistry::find(const std::string& name) const {
    auto hit = std::find_if(
        tools_.begin(), tools_.end(),
        [&](const std::unique_ptr<MaiTool>& candidate) { return candidate->name() == name; });
    return hit == tools_.end() ? nullptr : hit->get();
}

bool MaiToolRegistry::isEmpty() const {
    return tools_.empty();
}

std::vector<MaiToolSpec> MaiToolRegistry::specs() const {
    std::vector<MaiToolSpec> out;
    out.reserve(tools_.size());
    for (const auto& tool : tools_) {
        MaiToolSpec spec;
        spec.name = tool->name();
        spec.description = tool->description();
        spec.parametersJson = tool->parametersSchema();
        out.push_back(std::move(spec));
    }
    return out;
}

void registerMaiBuiltinTools(MaiToolRegistry& registry) {
    registry.add(makeMaiReadTool());
    registry.add(makeMaiWriteTool());
    registry.add(makeMaiGlobTool());
    registry.add(makeMaiGrepTool());
}
