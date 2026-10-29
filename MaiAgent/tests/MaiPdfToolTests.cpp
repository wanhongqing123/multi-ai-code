// 文件作用：验证“读取和处理 PDF 文档供 Agent 使用”的正确行为和边界条件。
// 验证入口：test_generates_pdf_directly_from_markdown、test_creates_validated_pdf_with_approval、
// test_rejects_paths_outside_workspace；检查可观察结果而非实现细节。
#include "MaiPdfTool.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

class MaiPdfWorkspace {
public:
    explicit MaiPdfWorkspace(bool createSource = true)
        : mRoot(MaiFileSystem::temporaryDirectory().append(
              MaiFilePath::fromUtf8(MaiIdGenerator::generate("pdf_tool_test_")))) {
        CHECK(!MaiFileSystem::createDirectories(mRoot));
        if (createSource) CHECK(!MaiFileSystem::writeFile(path("source.html"), "<h1>Report</h1>"));
    }

    ~MaiPdfWorkspace() {
        MaiFileSystem::removeRecursively(mRoot);
    }

    MaiFilePath path(const std::string& name) const {
        return mRoot.append(MaiFilePath::fromUtf8(name));
    }

    MaiToolContext context() const {
        MaiToolContext context;
        context.root = mRoot.toUtf8();
        return context;
    }

private:
    MaiFilePath mRoot;
};

void test_generates_pdf_directly_from_markdown() {
    MaiPdfWorkspace workspace(false);
    int renderCalls = 0;
    auto tool = makeMaiPdfTool(
        [&](const std::string& html, const std::string& output, const std::atomic<bool>*) {
            ++renderCalls;
            CHECK(html.find("<h1>Report</h1>") != std::string::npos);
            CHECK(html.find("<table>") != std::string::npos);
            CHECK(html.find("<strong>Passed</strong>") != std::string::npos);
            CHECK(html.find("<script>") == std::string::npos);
            const MaiError error =
                MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), "%PDF-1.4\n%%EOF\n");
            if (error) return MaiToolResult::failure(error.code(), error.message());
            return MaiToolResult::success("rendered");
        });
    const MaiToolResult result = tool->execute(
        R"({"content":"# Report\n\n| Item | Result |\n| --- | --- |\n| Build | **Passed** |\n\n<sc)"
        R"(ript>alert(1)</script>","output_path":"report.pdf"})",
        workspace.context());
    CHECK(!result.hasError());
    const auto artifact = nlohmann::json::parse(result.output(), nullptr, false);
    CHECK(artifact.is_object());
    if (artifact.is_object()) {
        CHECK(artifact.value("path", "") == "report.pdf");
        CHECK(artifact.value("mime_type", "") == "application/pdf");
        CHECK(artifact.value("bytes", 0) > 8);
    }
    CHECK(MaiFileSystem::exists(workspace.path("report.pdf")));
    CHECK(!MaiFileSystem::exists(workspace.path("source.html")));
    CHECK(renderCalls == 1);
}

void test_creates_validated_pdf_with_approval() {
    MaiPdfWorkspace workspace;
    int renderCalls = 0;
    auto tool = makeMaiPdfTool(
        [&](const std::string& html, const std::string& output, const std::atomic<bool>*) {
            ++renderCalls;
            CHECK(html == "<h1>Report</h1>");
            const MaiError error =
                MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), "%PDF-1.4\n%%EOF\n");
            if (error) return MaiToolResult::failure(error.code(), error.message());
            return MaiToolResult::success("rendered");
        });
    CHECK(tool != nullptr);
    CHECK(tool->name() == "generate_pdf");
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->parametersSchema().find("\"required\":[\"content\",\"output_path\"]") !=
          std::string::npos);
    const MaiToolResult result = tool->execute(
        R"({"source_path":"source.html","output_path":"report.pdf"})", workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("report.pdf") != std::string::npos);
    CHECK(MaiFileSystem::exists(workspace.path("report.pdf")));
    CHECK(renderCalls == 1);
    CHECK(tool->execute(R"({"source_path":"source.html","output_path":"report.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(renderCalls == 1);
}

void test_rejects_paths_outside_workspace() {
    MaiPdfWorkspace workspace;
    int renderCalls = 0;
    auto tool =
        makeMaiPdfTool([&](const std::string&, const std::string&, const std::atomic<bool>*) {
            ++renderCalls;
            return MaiToolResult::success("unreachable");
        });
    CHECK(tool->execute(R"({"source_path":"../secret.html","output_path":"report.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(tool->execute(R"({"source_path":"source.html","output_path":"../report.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(tool->execute(R"({"source_path":"source.html","output_path":"report.txt"})",
                        workspace.context())
              .hasError());
    CHECK(tool->execute(R"({"source_path":"source.html","output_path":"report.pdf","extra":1})",
                        workspace.context())
              .hasError());
    CHECK(tool->execute(R"({"content":"# Report","output_path":"../report.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(tool->execute(R"({"content":" ","output_path":"report.pdf"})", workspace.context())
              .hasError());
    CHECK(
        tool->execute(
                R"({"content":"# Report","source_path":"source.html","output_path":"report.pdf"})",
                workspace.context())
            .hasError());
    CHECK(renderCalls == 0);
}

void test_desktop_can_generate_pdf_outside_session_directory() {
    MaiPdfWorkspace session;
    MaiPdfWorkspace destination(false);
    auto tool = makeMaiPdfTool(
        [&](const std::string&, const std::string& output, const std::atomic<bool>*) {
            const MaiError error =
                MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), "%PDF-1.4\n%%EOF\n");
            return error ? MaiToolResult::failure(error.code(), error.message())
                         : MaiToolResult::success("rendered");
        });
    MaiToolContext context = session.context();
    context.allowOutsideWorkingDirectory = true;
    const std::string output = destination.path("outside.pdf").toUtf8();
    const auto arguments = nlohmann::json{{"content", "# Report"}, {"output_path", output}};
    const MaiToolResult result = tool->execute(arguments.dump(), context);
    CHECK(!result.hasError());
    CHECK(MaiFileSystem::exists(destination.path("outside.pdf")));
    CHECK(tool->approvalKeys(arguments.dump(), context) ==
          std::vector<std::string>{"file:" + context.resolvePath(output)});
}

void test_only_accepts_embedded_markdown_images() {
    MaiPdfWorkspace workspace(false);
    int renderCalls = 0;
    auto tool = makeMaiPdfTool(
        [&](const std::string& html, const std::string& output, const std::atomic<bool>*) {
            ++renderCalls;
            CHECK(html.find("<img src=\"data:image/png;base64,") != std::string::npos);
            const MaiError error =
                MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), "%PDF-1.4\n%%EOF\n");
            if (error) return MaiToolResult::failure(error.code(), error.message());
            return MaiToolResult::success("rendered");
        });
    CHECK(tool->execute(R"pdf({"content":"![image](https://example.com/image.png)",)pdf"
                        R"pdf("output_path":"external.pdf"})pdf",
                        workspace.context())
              .hasError());
    CHECK(renderCalls == 0);
    CHECK(!tool->execute(R"pdf({"content":"![image](data:image/png;base64,iVBORw0KGgo=)",)pdf"
                         R"pdf("output_path":"embedded.pdf"})pdf",
                         workspace.context())
               .hasError());
    CHECK(renderCalls == 1);
}

void test_rejects_missing_or_invalid_renderer_output() {
    MaiPdfWorkspace workspace;
    auto missing =
        makeMaiPdfTool([](const std::string&, const std::string&, const std::atomic<bool>*) {
            return MaiToolResult::success("rendered");
        });
    CHECK(missing
              ->execute(R"({"source_path":"source.html","output_path":"missing.pdf"})",
                        workspace.context())
              .hasError());
    auto invalid =
        makeMaiPdfTool([](const std::string&, const std::string& output, const std::atomic<bool>*) {
            CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), "not a PDF"));
            return MaiToolResult::success("rendered");
        });
    CHECK(invalid
              ->execute(R"({"source_path":"source.html","output_path":"invalid.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(!MaiFileSystem::exists(workspace.path("invalid.pdf")));
    CHECK(makeMaiPdfTool({}) == nullptr);
}

void test_rejects_invalid_utf8_and_renderer_exceptions() {
    MaiPdfWorkspace workspace;
    int renderCalls = 0;
    auto tool = makeMaiPdfTool(
        [&](const std::string&, const std::string&, const std::atomic<bool>*) -> MaiToolResult {
            ++renderCalls;
            throw std::runtime_error("renderer unavailable");
        });
    CHECK(!MaiFileSystem::writeFile(workspace.path("source.html"), "\xFF"));
    CHECK(tool->execute(R"({"source_path":"source.html","output_path":"bad.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(renderCalls == 0);
    CHECK(!MaiFileSystem::writeFile(workspace.path("source.html"), "<p>valid</p>"));
    CHECK(tool->execute(R"({"source_path":"source.html","output_path":"bad.pdf"})",
                        workspace.context())
              .hasError());
    CHECK(renderCalls == 1);
}

// 私有输入与 Agent 参数分离；渲染期间最终路径不存在，成功只公布已加密的文件。
void test_private_password_generation_and_cancel() {
    MaiPdfWorkspace workspace(false);
    const std::string fixturePassword = "Public-Pdf-Test-2026!";
    int promptCalls = 0;
    int renderCalls = 0;
    auto renderer = [&](const std::string&, const std::string& output, const std::atomic<bool>*) {
        ++renderCalls;
        CHECK(!MaiFileSystem::exists(workspace.path("protected.pdf")));
        CHECK(output != workspace.path("protected.pdf").toUtf8());
        std::string pdf = "%PDF-1.7\n";
        const std::vector<std::string> objects{
            "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << >> >>"};
        std::vector<std::size_t> offsets;
        for (std::size_t index = 0; index < objects.size(); ++index) {
            offsets.push_back(pdf.size());
            pdf += std::to_string(index + 1) + " 0 obj\n" + objects[index] + "\nendobj\n";
        }
        const auto xref = pdf.size();
        pdf += "xref\n0 4\n0000000000 65535 f \n";
        for (const auto offset : offsets) {
            char line[32]{};
            std::snprintf(line, sizeof(line), "%010llu 00000 n \n",
                          static_cast<unsigned long long>(offset));
            pdf += line;
        }
        pdf +=
            "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        const auto error = MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output), pdf);
        return error ? MaiToolResult::failure(error.code(), error.message())
                     : MaiToolResult::success("rendered");
    };
    auto tool = makeMaiPdfTool(renderer, [&](const std::atomic<bool>*) -> MaiResult<std::string> {
        ++promptCalls;
        return fixturePassword;
    });
    CHECK(tool->parametersSchema().find("password_protected") != std::string::npos);
    CHECK(tool->parametersSchema().find("\"password\":") == std::string::npos);
    CHECK(
        tool->execute(
                R"({"content":"Report","output_path":"protected.pdf","password":"must-not-be-accepted"})",
                workspace.context())
            .hasError());
    CHECK(promptCalls == 0 && renderCalls == 0);
    const auto result = tool->execute(
        R"({"content":"Report","output_path":"protected.pdf","password_protected":true})",
        workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find(fixturePassword) == std::string::npos);
    CHECK(promptCalls == 1 && renderCalls == 1);
    CHECK(MaiFileSystem::exists(workspace.path("protected.pdf")));
    auto canceled =
        makeMaiPdfTool(renderer, [](const std::atomic<bool>*) -> MaiResult<std::string> {
            return {MaiErrorCode::Canceled, "private input canceled"};
        });
    CHECK(canceled
              ->execute(
                  R"({"content":"Report","output_path":"cancel.pdf","password_protected":true})",
                  workspace.context())
              .error()
              .code() == MaiErrorCode::Canceled);
    CHECK(renderCalls == 1 && !MaiFileSystem::exists(workspace.path("cancel.pdf")));
    auto absent = makeMaiPdfTool(renderer);
    CHECK(absent
              ->execute(
                  R"({"content":"Report","output_path":"absent.pdf","password_protected":true})",
                  workspace.context())
              .error()
              .code() == MaiErrorCode::NotConfigured);
    CHECK(renderCalls == 1 && !MaiFileSystem::exists(workspace.path("absent.pdf")));
}

}  // namespace

int main() {
    test_generates_pdf_directly_from_markdown();
    test_creates_validated_pdf_with_approval();
    test_rejects_paths_outside_workspace();
    test_desktop_can_generate_pdf_outside_session_directory();
    test_only_accepts_embedded_markdown_images();
    test_rejects_missing_or_invalid_renderer_output();
    test_rejects_invalid_utf8_and_renderer_exceptions();
    test_private_password_generation_and_cancel();
    if (failures == 0) std::printf("PDF tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
