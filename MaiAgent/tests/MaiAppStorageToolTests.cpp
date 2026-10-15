// 文件作用：验证“提供应用私有存储的查询与文件操作工具”的正确行为和边界条件。
// 验证入口：阅读 main 或测试注册处，核对输入、预期结果与失败输出。
#include "MaiAppStorageTool.h"

#include <json.hpp>

#include <cstdio>
#include <ctime>
#include <string>

#ifndef _WIN32
#include <utime.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

}  // namespace

// 按应用私有目录的读写、路径保护和缺失文件场景验证工具，不访问用户真实文件。
int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("storage_test_")));
    const MaiFilePath temporary = root.append(MaiFilePath::fromUtf8("tmp"));
    const MaiFilePath cache = root.append(MaiFilePath::fromUtf8("cache"));
    const MaiFilePath workspace = root.append(MaiFilePath::fromUtf8("workspace"));
    CHECK(!MaiFileSystem::createDirectories(temporary));
    CHECK(!MaiFileSystem::createDirectories(cache));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const MaiFilePath oldFile = temporary.append(MaiFilePath::fromUtf8("old.tmp"));
    const MaiFilePath currentFile = cache.append(MaiFilePath::fromUtf8("current.tmp"));
    const MaiFilePath workFile = workspace.append(MaiFilePath::fromUtf8("keep.txt"));
    CHECK(!MaiFileSystem::writeFile(oldFile, "old"));
    CHECK(!MaiFileSystem::writeFile(currentFile, "current"));
    CHECK(!MaiFileSystem::writeFile(workFile, "keep"));
#ifndef _WIN32
    const std::time_t oldTime = std::time(nullptr) - 26 * 60 * 60;
    utimbuf times{oldTime, oldTime};
    CHECK(::utime(oldFile.value().c_str(), &times) == 0);
#endif
    MaiToolContext context;
    context.sessionId = "test-session";
    auto tool = makeMaiAppStorageTool({temporary.toUtf8(), cache.toUtf8(), workspace.toUtf8()});
    CHECK(!tool->requiresPerCallApproval(R"({"action":"scan"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"cleanup","preview_id":"x"})"));
    const auto scanned = tool->execute(R"({"action":"scan"})", context);
    CHECK(!scanned.hasError());
    if (!scanned.hasError()) {
        const auto result = nlohmann::json::parse(scanned.output());
        CHECK(result["temporary"]["bytes"] == 3);
        CHECK(result["workspace"]["bytes"] == 4);
#ifndef _WIN32
        const auto cleanup = tool->execute(
            nlohmann::json{{"action", "cleanup"}, {"preview_id", result["preview_id"]}}.dump(),
            context);
        CHECK(!cleanup.hasError());
        if (!cleanup.hasError())
            CHECK(nlohmann::json::parse(cleanup.output())["removed_files"] == 1);
        CHECK(!MaiFileSystem::exists(oldFile));
        CHECK(MaiFileSystem::exists(currentFile));
        CHECK(MaiFileSystem::exists(workFile));
        CHECK(tool->execute(
                      nlohmann::json{{"action", "cleanup"}, {"preview_id", result["preview_id"]}}
                          .dump(),
                      context)
                  .hasError());
#endif
    }
    MaiFileSystem::removeRecursively(root);
    return failures ? 1 : 0;
}
