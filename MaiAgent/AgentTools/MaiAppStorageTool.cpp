#include "MaiAppStorageTool.h"

#include <json.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumFiles = 20000;
constexpr std::size_t kMaximumCleanupFiles = 1000;
constexpr std::int64_t kMinimumAgeSeconds = 24 * 60 * 60;

struct Candidate {
    MaiFilePath path;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
};

struct Scan {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t eligibleFiles = 0;
    std::uint64_t eligibleBytes = 0;
    bool truncated = false;
    std::vector<Candidate> candidates;
};

Scan scanDirectory(const MaiFilePath& root, std::int64_t cutoff, std::size_t candidateLimit,
                   bool cleanupEligible, const MaiToolContext& context) {
    Scan result;
    if (root.isEmpty() || !MaiFileSystem::isDirectory(root)) return result;
    MaiFileSystem::walk(root, [&](const MaiFileEntry& entry) {
        if (context.isCanceled()) {
            result.truncated = true;
            return MaiWalkAction::Stop;
        }
        if (result.files >= kMaximumFiles) {
            result.truncated = true;
            return MaiWalkAction::Stop;
        }
        if (entry.isDirectory) return MaiWalkAction::Continue;
        if (MaiFileSystem::isSymbolicLink(entry.path)) return MaiWalkAction::Continue;
        ++result.files;
        result.bytes += entry.size;
        std::int64_t modified = 0;
        if (cleanupEligible && MaiFileSystem::modifiedTime(entry.path, modified) && modified > 0 &&
            modified <= cutoff) {
            ++result.eligibleFiles;
            result.eligibleBytes += entry.size;
            if (result.candidates.size() < candidateLimit)
                result.candidates.push_back({entry.path, entry.size, modified});
        }
        return MaiWalkAction::Continue;
    });
    return result;
}

Json summary(const Scan& scan) {
    return Json{{"bytes", scan.bytes},
                {"files", scan.files},
                {"scan_truncated", scan.truncated},
                {"eligible_files", scan.eligibleFiles},
                {"eligible_bytes", scan.eligibleBytes},
                {"cleanup_limited", scan.eligibleFiles > scan.candidates.size()},
                {"cleanup_candidates", scan.candidates.size()}};
}

class AppStorageTool final : public MaiTool {
public:
    explicit AppStorageTool(MaiAppStoragePaths paths) : mPaths(std::move(paths)) {}

    std::string name() const override {
        return "app_storage";
    }
    std::string description() const override {
        // 清理前先扫描并取得预览 ID；只删除符合该预览且未变化的旧缓存，
        // 不能把整个工作区或用户原文件当成临时文件清空。
        return "Inspect App temporary, cache and AI workspace storage. Scan returns sizes and a "
               "preview ID. Cleanup accepts that ID and removes only unchanged regular files "
               "older than 24 hours from App temporary/cache directories, never the workspace. "
               "Cleanup requires fresh approval each time.";
    }
    std::string parametersSchema() const override {
        // action=scan 只读并返回预览 ID；action=cleanup 要带这次预览 ID，
        // 执行层据此确认待删文件未变化。
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["scan","cleanup"]},"preview_id":{"type":"string"}},"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        return !args.is_object() || !args.value("action", Json{}).is_string() ||
               args["action"].get<std::string>() != "scan";
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return requiresApproval(raw);
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "action is required");
        const std::string action = args["action"].get<std::string>();
        if (action == "scan") return scan(context);
        if (action == "cleanup") {
            if (!args.value("preview_id", Json{}).is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "preview_id is required");
            return cleanup(args["preview_id"].get<std::string>(), context);
        }
        return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unknown storage action");
    }

private:
    MaiToolResult scan(const MaiToolContext& context) {
        const std::string workspacePath =
            mPaths.workspace.empty() ? context.root : mPaths.workspace;
        const MaiFilePath temporaryRoot =
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(mPaths.temporary));
        const MaiFilePath cacheRoot = MaiFileSystem::resolve(MaiFilePath::fromUtf8(mPaths.cache));
        const MaiFilePath workspaceRoot =
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(workspacePath));
        if (temporaryRoot.isEmpty() || cacheRoot.isEmpty() || workspaceRoot.isEmpty() ||
            !MaiFileSystem::isDirectory(temporaryRoot) || !MaiFileSystem::isDirectory(cacheRoot) ||
            !MaiFileSystem::isDirectory(workspaceRoot) || temporaryRoot == cacheRoot ||
            temporaryRoot.isParentOf(cacheRoot) || cacheRoot.isParentOf(temporaryRoot) ||
            temporaryRoot == workspaceRoot || cacheRoot == workspaceRoot ||
            temporaryRoot.isParentOf(workspaceRoot) || cacheRoot.isParentOf(workspaceRoot) ||
            workspaceRoot.isParentOf(temporaryRoot) || workspaceRoot.isParentOf(cacheRoot))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "App storage roots are missing or overlap");
        const std::int64_t cutoff =
            static_cast<std::int64_t>(std::time(nullptr)) - kMinimumAgeSeconds;
        const Scan temporary =
            scanDirectory(temporaryRoot, cutoff, kMaximumCleanupFiles, true, context);
        const Scan cache = scanDirectory(
            cacheRoot, cutoff, kMaximumCleanupFiles - temporary.candidates.size(), true, context);
        const Scan workspace = scanDirectory(workspaceRoot, cutoff, 0, false, context);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "storage scan was canceled");
        std::lock_guard<std::mutex> lock(mMutex);
        mSessionId = context.sessionId;
        mPreviewId = MaiIdGenerator::generate("storage_");
        mCreatedAt = static_cast<std::int64_t>(std::time(nullptr));
        mTemporaryRoot = temporaryRoot;
        mCacheRoot = cacheRoot;
        mCandidates = temporary.candidates;
        mCandidates.insert(mCandidates.end(), cache.candidates.begin(), cache.candidates.end());
        Json examples = Json::array();
        for (std::size_t index = 0; index < std::min<std::size_t>(mCandidates.size(), 20); ++index)
            examples.push_back(Json{{"path", mCandidates[index].path.toUtf8()},
                                    {"bytes", mCandidates[index].bytes}});
        Json response{{"preview_id", mPreviewId},
                      {"temporary", summary(temporary)},
                      {"cache", summary(cache)},
                      {"workspace", summary(workspace)},
                      {"cleanup_max_age_hours", 24},
                      {"cleanup_limit", kMaximumCleanupFiles},
                      {"cleanup_candidate_count", mCandidates.size()},
                      {"cleanup_candidate_examples", examples},
                      {"workspace_cleanup_allowed", false}};
        return MaiToolResult::success(response.dump());
    }

    MaiToolResult cleanup(const std::string& previewId, const MaiToolContext& context) {
        std::vector<Candidate> candidates;
        MaiFilePath temporaryRoot;
        MaiFilePath cacheRoot;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            if (previewId.empty() || previewId != mPreviewId || context.sessionId != mSessionId ||
                now - mCreatedAt > 10 * 60)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "scan storage again for a fresh preview ID");
            candidates = std::move(mCandidates);
            temporaryRoot = mTemporaryRoot;
            cacheRoot = mCacheRoot;
            mPreviewId.clear();
        }
        if (MaiFileSystem::resolve(MaiFilePath::fromUtf8(mPaths.temporary)) != temporaryRoot ||
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(mPaths.cache)) != cacheRoot)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "App storage roots changed; scan again");
        std::uint64_t bytes = 0;
        std::uint64_t removed = 0;
        std::uint64_t skipped = 0;
        for (const Candidate& item : candidates) {
            if (context.isCanceled())
                return MaiToolResult::failure(MaiErrorCode::Canceled,
                                              "storage cleanup was canceled");
            std::uint64_t currentSize = 0;
            std::int64_t modified = 0;
            const MaiFilePath resolved = MaiFileSystem::resolve(item.path);
            if (MaiFileSystem::isSymbolicLink(item.path) || resolved.isEmpty() ||
                (!temporaryRoot.isParentOf(resolved) && !cacheRoot.isParentOf(resolved)) ||
                !MaiFileSystem::fileSize(item.path, currentSize) ||
                !MaiFileSystem::modifiedTime(item.path, modified) || currentSize != item.bytes ||
                modified != item.modified ||
                modified > static_cast<std::int64_t>(std::time(nullptr)) - kMinimumAgeSeconds ||
                MaiFileSystem::removeFile(item.path).hasError()) {
                ++skipped;
                continue;
            }
            ++removed;
            bytes += currentSize;
        }
        return MaiToolResult::success(Json{
            {"removed_files", removed},
            {"reclaimed_bytes", bytes},
            {"skipped_files", skipped}}.dump());
    }

    MaiAppStoragePaths mPaths;
    std::mutex mMutex;
    std::string mSessionId;
    std::string mPreviewId;
    std::int64_t mCreatedAt = 0;
    MaiFilePath mTemporaryRoot;
    MaiFilePath mCacheRoot;
    std::vector<Candidate> mCandidates;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiAppStorageTool(MaiAppStoragePaths paths) {
    return std::make_unique<AppStorageTool>(std::move(paths));
}
