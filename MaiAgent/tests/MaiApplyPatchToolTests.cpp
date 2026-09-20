// apply_patch 的测试。
//
// 这个工具和 edit 的区别就在「全有或全无」上，所以用例的重心是**失败的那一半**：
// 一批改动里有一处对不上，剩下的必须一个字节都没落盘。半应用的补丁比不应用糟得多——
// 模型和用户都说不清当前是什么状态，而 git diff 里会混着真改动和半截改动。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <json.hpp>

#include "MaiTool.h"

namespace fs = std::filesystem;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

struct Workspace {
    fs::path root;

    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-patch-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& name, const std::string& body) const {
        const fs::path target = root / name;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << body;
    }

    std::string read(const std::string& name) const {
        std::ifstream in(root / name, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    bool has(const std::string& name) const {
        return fs::exists(root / name);
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_patch";
        c.root = root.string();
        return c;
    }
};

std::string patchArgs(const std::string& patch) {
    return json({{"patch", patch}}).dump();
}

void test_updates_several_places_in_one_file() {
    Workspace workspace;
    workspace.write("a.txt", "a\nb\nc\nd\ne\nf\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: a.txt\n"
                                                   "@@\n"
                                                   " a\n"
                                                   "-b\n"
                                                   "+B\n"
                                                   "@@\n"
                                                   " d\n"
                                                   "-e\n"
                                                   "+E\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("a.txt") == "a\nB\nc\nd\nE\nf\n");
}

void test_one_patch_can_touch_several_files() {
    // 这是它相对 edit 的全部意义：改接口同时改所有调用方，一次提交。
    Workspace workspace;
    workspace.write("one.txt", "old\n");
    workspace.write("two.txt", "old\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: one.txt\n"
                                                   "@@\n"
                                                   "-old\n"
                                                   "+new\n"
                                                   "*** Update File: two.txt\n"
                                                   "@@\n"
                                                   "-old\n"
                                                   "+new\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("one.txt") == "new\n");
    CHECK(workspace.read("two.txt") == "new\n");
}

void test_nothing_is_written_when_any_part_fails() {
    // **这条是这个工具存在的理由。** 第一个文件改得动，第二个对不上，
    // 那第一个也不许落盘。
    Workspace workspace;
    workspace.write("good.txt", "old\n");
    workspace.write("bad.txt", "something else\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: good.txt\n"
                                                   "@@\n"
                                                   "-old\n"
                                                   "+new\n"
                                                   "*** Update File: bad.txt\n"
                                                   "@@\n"
                                                   "-this line is not in the file\n"
                                                   "+whatever\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(result.hasError());
    CHECK(workspace.read("good.txt") == "old\n");
    CHECK(workspace.read("bad.txt") == "something else\n");
    // 报错要说清是哪一处对不上，并且明说什么都没改。
    CHECK(result.error().message().find("Nothing was changed") != std::string::npos);
}

void test_add_and_delete_files() {
    Workspace workspace;
    workspace.write("gone.txt", "bye\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Add File: made/up.txt\n"
                                                   "+first\n"
                                                   "+second\n"
                                                   "*** Delete File: gone.txt\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("made/up.txt") == "first\nsecond\n");
    CHECK(!workspace.has("gone.txt"));
    // 报给模型的状态要分得清增删改。
    CHECK(result.output().find("A made/up.txt") != std::string::npos);
    CHECK(result.output().find("D gone.txt") != std::string::npos);
}

void test_add_refuses_to_clobber_an_existing_file() {
    // Add 撞上已有文件时静默覆盖的话，一整个文件的内容就没了。
    Workspace workspace;
    workspace.write("there.txt", "precious\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Add File: there.txt\n"
                                                   "+overwritten\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(result.hasError());
    CHECK(workspace.read("there.txt") == "precious\n");
    CHECK(result.error().message().find("already exists") != std::string::npos);
}

void test_move_writes_the_new_path_and_removes_the_old() {
    Workspace workspace;
    workspace.write("from.txt", "keep\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: from.txt\n"
                                                   "*** Move to: to.txt\n"
                                                   "@@\n"
                                                   "-keep\n"
                                                   "+moved\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("to.txt") == "moved\n");
    CHECK(!workspace.has("from.txt"));
}

void test_crlf_files_keep_their_line_endings() {
    // **Windows 上最容易踩的一脚。** 把 CRLF 的文件写成 LF，git 会把整个文件
    // 报成改动过，真正的那几行淹没在里面。
    Workspace workspace;
    workspace.write("crlf.txt", "a\r\nb\r\nc\r\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: crlf.txt\n"
                                                   "@@\n"
                                                   " a\n"
                                                   "-b\n"
                                                   "+B\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("crlf.txt") == "a\r\nB\r\nc\r\n");
}

void test_context_is_matched_leniently_on_whitespace() {
    // 模型抄上下文时最常见的偏差就是行尾空格。一次都不让步的话大部分补丁都会失败，
    // 而失败的理由（少了一个空格）它自己看不出来。三级放松和 codex 的 seek_sequence 一致。
    Workspace workspace;
    workspace.write("ws.txt", "keep   \n  target\nafter\n");
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: ws.txt\n"
                                                   "@@\n"
                                                   " keep\n"
                                                   "-  target\n"
                                                   "+  replaced\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("ws.txt").find("replaced") != std::string::npos);
}

void test_malformed_patches_are_rejected_clearly() {
    Workspace workspace;
    workspace.write("x.txt", "a\n");
    auto tool = makeMaiApplyPatchTool();

    // 没有信封
    CHECK(tool->execute(patchArgs("*** Update File: x.txt\n@@\n-a\n+b\n"), workspace.context())
              .hasError());
    // 没有收尾
    CHECK(tool->execute(patchArgs("*** Begin Patch\n*** Update File: x.txt\n@@\n-a\n+b\n"),
                        workspace.context())
              .hasError());
    // 空补丁
    CHECK(tool->execute(patchArgs("*** Begin Patch\n*** End Patch\n"), workspace.context())
              .hasError());
    // 改动行前面没有标记字符，分不清是上下文还是别的什么
    CHECK(tool->execute(patchArgs("*** Begin Patch\n*** Update File: x.txt\n@@\nbare line\n"
                                  "*** End Patch\n"),
                        workspace.context())
              .hasError());
    // 一个都不许落盘
    CHECK(workspace.read("x.txt") == "a\n");
}

void test_cannot_escape_the_working_directory() {
    // 路径边界是安全边界。每个碰文件的工具都要自己被验一遍，
    // 不能假设别人验过。
    Workspace workspace;
    auto tool = makeMaiApplyPatchTool();

    const MaiToolResult update = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: ../outside.txt\n"
                                                   "@@\n"
                                                   "-a\n"
                                                   "+b\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(update.hasError());
    CHECK(update.error().message().find("outside the working directory") != std::string::npos);

    // 新建和移动的目标路径同样要挡。
    const MaiToolResult add = tool->execute(patchArgs(
                                                "*** Begin Patch\n"
                                                "*** Add File: ../escaped.txt\n"
                                                "+x\n"
                                                "*** End Patch\n"),
                                            workspace.context());
    CHECK(add.hasError());
    CHECK(!fs::exists(workspace.root.parent_path() / "escaped.txt"));
}

void test_missing_file_is_reported_as_not_found() {
    Workspace workspace;
    auto tool = makeMaiApplyPatchTool();
    const MaiToolResult result = tool->execute(patchArgs(
                                                   "*** Begin Patch\n"
                                                   "*** Update File: nope.txt\n"
                                                   "@@\n"
                                                   "-a\n"
                                                   "+b\n"
                                                   "*** End Patch\n"),
                                               workspace.context());
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotFound);
}

void test_apply_patch_always_needs_approval() {
    auto tool = makeMaiApplyPatchTool();
    CHECK(tool->requiresApproval(patchArgs("*** Begin Patch\n*** End Patch\n")));
    CHECK(tool->requiresApproval("not json at all"));
    CHECK(tool->approvalKey(patchArgs("anything")) == "apply_patch");
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_updates_several_places_in_one_file);
    RUN(test_one_patch_can_touch_several_files);
    RUN(test_nothing_is_written_when_any_part_fails);
    RUN(test_add_and_delete_files);
    RUN(test_add_refuses_to_clobber_an_existing_file);
    RUN(test_move_writes_the_new_path_and_removes_the_old);
    RUN(test_crlf_files_keep_their_line_endings);
    RUN(test_context_is_matched_leniently_on_whitespace);
    RUN(test_malformed_patches_are_rejected_clearly);
    RUN(test_cannot_escape_the_working_directory);
    RUN(test_missing_file_is_reported_as_not_found);
    RUN(test_apply_patch_always_needs_approval);
    if (failures == 0) std::printf("apply_patch tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
