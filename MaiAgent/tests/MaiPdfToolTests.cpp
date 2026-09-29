#include "MaiPdfTool.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

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
        R"({"content":"# Report\n\n| Item | Result |\n| --- | --- |\n| Build | **Passed** |\n\n<script>alert(1)</script>","output_path":"report.pdf"})",
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
    CHECK(
        tool->execute(
                R"pdf({"content":"![image](https://example.com/image.png)","output_path":"external.pdf"})pdf",
                workspace.context())
            .hasError());
    CHECK(renderCalls == 0);
    CHECK(
        !tool->execute(
                 R"pdf({"content":"![image](data:image/png;base64,iVBORw0KGgo=)","output_path":"embedded.pdf"})pdf",
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

}  // namespace

int main() {
    test_generates_pdf_directly_from_markdown();
    test_creates_validated_pdf_with_approval();
    test_rejects_paths_outside_workspace();
    test_only_accepts_embedded_markdown_images();
    test_rejects_missing_or_invalid_renderer_output();
    test_rejects_invalid_utf8_and_renderer_exceptions();
    if (failures == 0) std::printf("PDF tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
