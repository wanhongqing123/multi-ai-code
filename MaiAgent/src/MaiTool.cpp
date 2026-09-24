#include "MaiTool.h"

#include <algorithm>

#include "MaiProcess.h"
#include "MaiApplyPatchTool.h"
#include "MaiEditTool.h"
#include "MaiFileTools.h"
#include "MaiQuestionTool.h"
#include "MaiScreenshot.h"
#include "MaiScreenshotTool.h"
#include "MaiShellTool.h"
#include "MaiSubAgentTools.h"
#include "MaiTimeTool.h"
#include "MaiTodoWriteTool.h"
#include "MaiWebFetchTool.h"

// ── MaiToolContext ──────────────────────────────────────────────

bool MaiToolContext::isCanceled() const {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

// ── MaiToolResult ───────────────────────────────────────────────

MaiToolResult MaiToolResult::success(std::string output, bool truncated) {
    MaiToolResult result;
    result.mOutput = std::move(output);
    result.mTruncated = truncated;
    return result;
}

MaiToolResult MaiToolResult::successWithImages(std::string output,
                                               std::vector<MaiToolImage> images) {
    MaiToolResult result;
    result.mOutput = std::move(output);
    result.mImages = std::move(images);
    return result;
}

MaiToolResult MaiToolResult::failure(MaiErrorCode code, std::string message) {
    MaiToolResult result;
    result.mError = MaiError::make(code, std::move(message));
    return result;
}

const std::string& MaiToolResult::output() const {
    return mOutput;
}

const std::vector<MaiToolImage>& MaiToolResult::images() const {
    return mImages;
}

const MaiError& MaiToolResult::error() const {
    return mError;
}

bool MaiToolResult::isTruncated() const {
    return mTruncated;
}

bool MaiToolResult::hasError() const {
    return mError.hasError();
}

// ── MaiTool ─────────────────────────────────────────────────────

std::string MaiTool::approvalKey(const std::string& argumentsJson) const {
    (void)argumentsJson;
    return name();
}

bool MaiTool::requiresApproval(const std::string& argumentsJson) const {
    (void)argumentsJson;
    return false;
}

// ── MaiToolRegistry ─────────────────────────────────────────────

void MaiToolRegistry::add(std::unique_ptr<MaiTool> tool) {
    if (!tool) return;
    const std::string name = tool->name();
    // 同名覆盖：注册表里不该出现两个同名工具，模型按名字调，撞了就不确定跑哪个。
    auto existing = std::find_if(
        mTools.begin(), mTools.end(),
        [&](const std::unique_ptr<MaiTool>& candidate) { return candidate->name() == name; });
    if (existing != mTools.end()) {
        *existing = std::move(tool);
        return;
    }
    mTools.push_back(std::move(tool));
}

MaiTool* MaiToolRegistry::find(const std::string& name) const {
    auto hit = std::find_if(
        mTools.begin(), mTools.end(),
        [&](const std::unique_ptr<MaiTool>& candidate) { return candidate->name() == name; });
    return hit == mTools.end() ? nullptr : hit->get();
}

bool MaiToolRegistry::isEmpty() const {
    return mTools.empty();
}

std::vector<MaiToolSpec> MaiToolRegistry::specs() const {
    std::vector<MaiToolSpec> out;
    out.reserve(mTools.size());
    for (const auto& tool : mTools) {
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
    registry.add(makeMaiEditTool());
    registry.add(makeMaiApplyPatchTool());
    registry.add(makeMaiTodoWriteTool());
    registry.add(makeMaiWebFetchTool());
    registry.add(makeMaiQuestionTool());
    registry.add(makeMaiSpawnAgentTool());
    registry.add(makeMaiWaitAgentTool());
    registry.add(makeMaiSendInputTool());
    registry.add(makeMaiListAgentsTool());
    registry.add(makeMaiCloseAgentTool());
    registry.add(makeMaiGlobTool());
    registry.add(makeMaiGrepTool());
    if (maiIsScreenshotSupported()) registry.add(makeMaiScreenshotTool());
    // shell 只在跑得了外部进程的平台上摆出来。
    //
    // **不支持就根本不注册，而不是注册一个总是失败的。** 摆出来的话模型会反复试，
    // 而它收到的「失败」听起来像临时故障，于是它换个写法再试一遍，一轮对话就耗在这上面了。
    // iOS 的沙箱不允许 exec，这是 App Store 的硬规矩（见 MaiProcess.h）。
    if (maiIsProcessExecutionSupported()) registry.add(makeMaiShellTool());
    registry.add(makeMaiCurrentTimeTool());
}
