// edit 工具的测试。
//
// 这个工具的价值全在「拒绝」上：能改的时候改对不难，难的是**该拒绝的时候一定要拒绝**。
// 匹配到多处还照改，或者没匹配上却静默成功，都会让模型以为自己改成了，
// 然后基于这个错误前提继续往下做——等发现时已经错了好几步。
//
// 非 ASCII 的素材写成 \u 转义：规范要求代码里除注释外不出现中文，
// 而这几条用例测的就是非 ASCII 内容能不能原样往返。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <json.hpp>

#include "MaiTool.h"
#include "MaiEditTool.h"
#include "MaiTimeTool.h"

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

// 非 ASCII 的测试数据写成 \u 转义：规范要求代码里除注释外不出现中文。
// 这两条测的就是「改一行不能把文件里别的多字节字符弄坏」，所以字节本身一个都不能改。
//
//   kCjkLine     \u7b2c\u4e00\u884c   第一行
//   kCjkChanged  \u6539\u8fc7\u4e86   改过了
const char* kCjkLine = "\u7b2c\u4e00\u884c";
const char* kCjkChanged = "\u6539\u8fc7\u4e86";

struct Workspace {
    fs::path root;

    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-edit-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& name, const std::string& body) const {
        std::ofstream out(root / name, std::ios::binary);
        out << body;
    }

    std::string read(const std::string& name) const {
        std::ifstream in(root / name, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_edit";
        c.root = root.string();
        return c;
    }
};

std::string args(const json& j) {
    return j.dump();
}

void test_replaces_a_unique_match() {
    Workspace workspace;
    workspace.write("a.txt", "alpha\nbeta\ngamma\n");
    auto tool = makeMaiEditTool();

    const MaiToolResult result = tool->execute(
        args({{"path", "a.txt"}, {"old_string", "beta"}, {"new_string", "BETA"}}),
        workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("a.txt") == "alpha\nBETA\ngamma\n");
    // 回给模型的要带一小段上下文，它才能确认改在了想改的地方。
    CHECK(result.output().find("BETA") != std::string::npos);
}

void test_refuses_when_the_match_is_ambiguous() {
    // 这条是这个工具存在的理由之一。两处都匹配却照改第一处的话，
    // 模型会以为两处都改了。
    Workspace workspace;
    workspace.write("b.txt", "same\nother\nsame\n");
    auto tool = makeMaiEditTool();

    const MaiToolResult result = tool->execute(
        args({{"path", "b.txt"}, {"old_string", "same"}, {"new_string", "changed"}}),
        workspace.context());
    CHECK(result.hasError());
    // 文件必须一个字节都没动。
    CHECK(workspace.read("b.txt") == "same\nother\nsame\n");
    // 错误信息要写成模型能据此改正的样子：说清楚出现了几次、下一步该怎么办。
    CHECK(result.error().message().find("2 times") != std::string::npos);
    CHECK(result.error().message().find("replace_all") != std::string::npos);
}

void test_replace_all_changes_every_occurrence() {
    Workspace workspace;
    workspace.write("c.txt", "x\ny\nx\nz\nx\n");
    auto tool = makeMaiEditTool();

    const MaiToolResult result =
        tool->execute(args({{"path", "c.txt"},
                            {"old_string", "x"},
                            {"new_string", "w"},
                            {"replace_all", true}}),
                      workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("c.txt") == "w\ny\nw\nz\nw\n");
    CHECK(result.output().find("3 occurrences") != std::string::npos);
}

void test_refuses_when_nothing_matches() {
    Workspace workspace;
    workspace.write("d.txt", "hello\n");
    auto tool = makeMaiEditTool();

    const MaiToolResult result = tool->execute(
        args({{"path", "d.txt"}, {"old_string", "goodbye"}, {"new_string", "hi"}}),
        workspace.context());
    CHECK(result.hasError());
    CHECK(workspace.read("d.txt") == "hello\n");
    // 最常见的原因是缩进抄错或者凭记忆写，所以提示要往这个方向指。
    CHECK(result.error().message().find("indentation") != std::string::npos);
}

void test_refuses_empty_and_identical_strings() {
    Workspace workspace;
    workspace.write("e.txt", "body\n");
    auto tool = makeMaiEditTool();

    // 空的 old_string 会匹配到处都是，语义上等于「在开头插入」——那是 write 的活。
    const MaiToolResult empty = tool->execute(
        args({{"path", "e.txt"}, {"old_string", ""}, {"new_string", "x"}}), workspace.context());
    CHECK(empty.hasError());

    // 一模一样等于没改。放过去的话模型会以为自己做了事。
    const MaiToolResult same = tool->execute(
        args({{"path", "e.txt"}, {"old_string", "body"}, {"new_string", "body"}}),
        workspace.context());
    CHECK(same.hasError());
    CHECK(workspace.read("e.txt") == "body\n");
}

void test_cannot_escape_the_working_directory() {
    // 路径边界是安全边界，不是建议。这条和 MaiToolTests 里的重复是**故意的**：
    // 每个会碰文件的工具都要自己被验一遍，不能假设别人验过。
    Workspace workspace;
    workspace.write("f.txt", "inside\n");
    auto tool = makeMaiEditTool();

    const MaiToolResult result =
        tool->execute(args({{"path", "../outside.txt"}, {"old_string", "a"}, {"new_string", "b"}}),
                      workspace.context());
    CHECK(result.hasError());
    CHECK(result.error().message().find("outside the working directory") != std::string::npos);
}

void test_missing_file_and_directory_are_reported_differently() {
    Workspace workspace;
    fs::create_directories(workspace.root / "adir");
    auto tool = makeMaiEditTool();

    const MaiToolResult missing = tool->execute(
        args({{"path", "nope.txt"}, {"old_string", "a"}, {"new_string", "b"}}),
        workspace.context());
    CHECK(missing.hasError());
    CHECK(missing.error().code() == MaiErrorCode::NotFound);

    const MaiToolResult directory = tool->execute(
        args({{"path", "adir"}, {"old_string", "a"}, {"new_string", "b"}}), workspace.context());
    CHECK(directory.hasError());
    CHECK(directory.error().message().find("directory") != std::string::npos);
}

void test_keeps_non_ascii_bytes_intact() {
    // 改一行不能把文件里别的多字节字符弄坏。
    Workspace workspace;
    const std::string body = std::string(kCjkLine) + "\nkeep me\n";
    workspace.write("g.txt", body);
    auto tool = makeMaiEditTool();

    const MaiToolResult result = tool->execute(
        args({{"path", "g.txt"}, {"old_string", "keep me"}, {"new_string", kCjkChanged}}),
        workspace.context());
    CHECK(!result.hasError());
    CHECK(workspace.read("g.txt") == std::string(kCjkLine) + "\n" + kCjkChanged + "\n");
}

void test_edit_always_needs_approval() {
    auto tool = makeMaiEditTool();
    // 参数是什么都要问：edit 的危险程度不随参数变。
    CHECK(tool->requiresApproval(args({{"path", "a.txt"}})));
    CHECK(tool->requiresApproval("not json at all"));
    // 会话级豁免的键就是工具名——这个工具不需要更细的粒度。
    CHECK(tool->approvalKey(args({{"path", "a.txt"}})) == "edit");
}

void test_current_time_is_utc_iso8601() {
    auto tool = makeMaiCurrentTimeTool();
    MaiToolContext context;
    const MaiToolResult result = tool->execute("{}", context);
    CHECK(!result.hasError());
    const std::string text = result.output();
    // 形如 2026-09-20T15:04:05Z。**必须带 Z**：不带时区的时间戳传到别处就没法解释了。
    CHECK(text.size() == 20);
    CHECK(text[4] == '-' && text[7] == '-');
    CHECK(text[10] == 'T');
    CHECK(text[13] == ':' && text[16] == ':');
    CHECK(text.back() == 'Z');
    // 只读，不该要审批。
    CHECK(!tool->requiresApproval("{}"));
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_replaces_a_unique_match);
    RUN(test_refuses_when_the_match_is_ambiguous);
    RUN(test_replace_all_changes_every_occurrence);
    RUN(test_refuses_when_nothing_matches);
    RUN(test_refuses_empty_and_identical_strings);
    RUN(test_cannot_escape_the_working_directory);
    RUN(test_missing_file_and_directory_are_reported_differently);
    RUN(test_keeps_non_ascii_bytes_intact);
    RUN(test_edit_always_needs_approval);
    RUN(test_current_time_is_utc_iso8601);
    if (failures == 0) std::printf("edit tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
