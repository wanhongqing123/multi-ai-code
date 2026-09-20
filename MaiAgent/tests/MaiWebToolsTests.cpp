// webfetch 和 todowrite 的测试。
//
// webfetch 真要联网才能跑通全程，而单元测试不该依赖外网——所以这里测的是
// **不联网也能测的那部分**：协议限制、审批粒度、HTML 转文本。真正的 GET
// 由 MaiModelClientTests 那套本地假服务端的路子覆盖更合适，这里不重复搭。
//
// todowrite 是纯函数，全都能测。
#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiTool.h"

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

std::string args(const json& j) {
    return j.dump();
}

MaiToolContext bareContext() {
    MaiToolContext context;
    context.sessionId = "ses_web";
    return context;
}

// ── webfetch ────────────────────────────────────────────────────

void test_only_http_urls_are_accepted() {
    // curl 默认还认 file:// gopher:// smb://，一个 file:// 就能把「抓网页」
    // 变成「读任意本地文件」——那是绕过工作目录边界的路。
    auto tool = makeMaiWebFetchTool();
    for (const char* url : {"file:///etc/passwd", "ftp://example.com/x", "gopher://example.com",
                            "smb://server/share", "javascript:alert(1)", "not a url at all"}) {
        const MaiToolResult result = tool->execute(args({{"url", url}}), bareContext());
        CHECK(result.hasError());
        CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(tool->execute("{}", bareContext()).hasError());
}

void test_every_host_needs_approval_and_the_key_is_the_host() {
    // 这是唯一一个往外发数据的工具。被提示词注入诱导的模型可以把读到的内容
    // 编进 URL 发出去，所以每个新域名都要点一次头。
    auto tool = makeMaiWebFetchTool();
    CHECK(tool->requiresApproval(args({{"url", "https://example.com/a"}})));
    CHECK(tool->requiresApproval("not json at all"));

    // 豁免收到域名这一级：给一个文档站点头，不该连别的站一起放行。
    CHECK(tool->approvalKey(args({{"url", "https://docs.example.com/a"}})) ==
          "webfetch:docs.example.com");
    CHECK(tool->approvalKey(args({{"url", "https://docs.example.com/b?q=1"}})) ==
          "webfetch:docs.example.com");
    CHECK(tool->approvalKey(args({{"url", "https://attacker.example/leak"}})) !=
          tool->approvalKey(args({{"url", "https://docs.example.com/a"}})));

    // 端口、大小写、URL 里的用户名都要归一化掉，否则同一个站会攒出好几份豁免，
    // 用户以为放行过了其实没有。
    CHECK(tool->approvalKey(args({{"url", "https://Docs.Example.com:8443/x"}})) ==
          "webfetch:docs.example.com");
    CHECK(tool->approvalKey(args({{"url", "https://user:pw@docs.example.com/x"}})) ==
          "webfetch:docs.example.com");
    CHECK(tool->approvalKey(args({{"url", "garbage"}})) == "webfetch:<unknown>");
}

// ── todowrite ───────────────────────────────────────────────────

void test_formats_the_list_and_counts_progress() {
    auto tool = makeMaiTodoWriteTool();
    const MaiToolResult result =
        tool->execute(args({{"todos",
                             {{{"content", "read the code"}, {"status", "completed"}},
                              {{"content", "write the fix"}, {"status", "in_progress"}},
                              {{"content", "run the tests"}, {"status", "pending"}}}}}),
                      bareContext());
    CHECK(!result.hasError());
    CHECK(result.output().find("[x] read the code") != std::string::npos);
    CHECK(result.output().find("[~] write the fix") != std::string::npos);
    CHECK(result.output().find("[ ] run the tests") != std::string::npos);
    CHECK(result.output().find("1 of 3 done") != std::string::npos);
}

void test_only_one_step_may_be_in_progress() {
    // 同时干好几件事正是跑偏的开始。
    auto tool = makeMaiTodoWriteTool();
    const MaiToolResult result =
        tool->execute(args({{"todos",
                             {{{"content", "a"}, {"status", "in_progress"}},
                              {{"content", "b"}, {"status", "in_progress"}}}}}),
                      bareContext());
    CHECK(result.hasError());
    // 报错要说清为什么，它下一圈才改得对。
    CHECK(result.error().message().find("one item") != std::string::npos);
    CHECK(result.error().message().find("2") != std::string::npos);
}

void test_rejects_malformed_lists() {
    auto tool = makeMaiTodoWriteTool();
    // 没有 todos
    CHECK(tool->execute("{}", bareContext()).hasError());
    // todos 不是数组
    CHECK(tool->execute(args({{"todos", "not an array"}}), bareContext()).hasError());
    // 空清单
    CHECK(tool->execute(args({{"todos", json::array()}}), bareContext()).hasError());
    // 状态词不认识
    CHECK(tool->execute(args({{"todos", {{{"content", "a"}, {"status", "maybe"}}}}}),
                        bareContext())
              .hasError());
    // 内容为空
    CHECK(tool->execute(args({{"todos", {{{"content", ""}, {"status", "pending"}}}}}),
                        bareContext())
              .hasError());
    // 参数根本不是 JSON
    CHECK(tool->execute("not json at all", bareContext()).hasError());
}

void test_todowrite_needs_no_approval() {
    // 它什么都不改，只是把清单摆出来。要审批的话模型就不会用了，
    // 而这个工具的价值恰恰在于它用得够勤。
    auto tool = makeMaiTodoWriteTool();
    CHECK(!tool->requiresApproval(args({{"todos", json::array()}})));
    CHECK(!tool->requiresApproval("not json at all"));
}

void test_both_tools_are_registered() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK(registry.find("webfetch") != nullptr);
    CHECK(registry.find("todowrite") != nullptr);
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main() {
    RUN(test_only_http_urls_are_accepted);
    RUN(test_every_host_needs_approval_and_the_key_is_the_host);
    RUN(test_formats_the_list_and_counts_progress);
    RUN(test_only_one_step_may_be_in_progress);
    RUN(test_rejects_malformed_lists);
    RUN(test_todowrite_needs_no_approval);
    RUN(test_both_tools_are_registered);
    if (failures == 0) std::printf("web tools tests passed\n");
    return failures == 0 ? 0 : 1;
}
