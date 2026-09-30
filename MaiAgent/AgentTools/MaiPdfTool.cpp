#include "MaiPdfTool.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>
#include <md4c-html.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kMaxInputBytes = 10u * 1024 * 1024;
constexpr std::uint64_t kMaxPdfBytes = 100u * 1024 * 1024;
constexpr std::size_t kMaxRenderedHtmlBytes = 20u * 1024 * 1024;

struct MaiMarkdownHtmlOutput {
    std::string html;
    bool isTooLarge = false;
};

void appendMarkdownHtml(const MD_CHAR* chunk, MD_SIZE length, void* context) {
    auto* output = static_cast<MaiMarkdownHtmlOutput*>(context);
    if (output->isTooLarge) return;
    if (length > kMaxRenderedHtmlBytes - output->html.size()) {
        output->isTooLarge = true;
        return;
    }
    output->html.append(chunk, length);
}

bool hasExternalImage(const std::string& html) {
    const std::string marker = "<img src=\"";
    std::size_t cursor = 0;
    while ((cursor = html.find(marker, cursor)) != std::string::npos) {
        const std::size_t start = cursor + marker.size();
        const std::size_t end = html.find('"', start);
        const std::size_t encoded = html.find(";base64,", start);
        if (end == std::string::npos || html.compare(start, 11, "data:image/") != 0 ||
            encoded == std::string::npos || encoded >= end)
            return true;
        cursor = end + 1;
    }
    return false;
}

bool markdownToHtml(const std::string& markdown, std::string& html) {
    MaiMarkdownHtmlOutput output;
    output.html.reserve(std::min(markdown.size() * 2, kMaxRenderedHtmlBytes));
    const int status = md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()),
                               appendMarkdownHtml, &output, MD_DIALECT_GITHUB | MD_FLAG_NOHTML,
                               MD_HTML_FLAG_XHTML | MD_HTML_FLAG_SKIP_UTF8_BOM);
    if (status != 0 || output.isTooLarge || hasExternalImage(output.html)) return false;
    html = "<html><head><meta charset=\"utf-8\"></head><body>";
    html += output.html;
    html += "</body></html>";
    return true;
}

bool hasExtension(const std::string& path, const std::string& extension) {
    if (path.size() < extension.size()) return false;
    return std::equal(extension.rbegin(), extension.rend(), path.rbegin(),
                      [](unsigned char expected, unsigned char actual) {
                          return expected == static_cast<unsigned char>(std::tolower(actual));
                      });
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first < 0x80) {
            ++index;
            continue;
        }
        int width = 0;
        if (first >= 0xC2 && first <= 0xDF)
            width = 2;
        else if (first >= 0xE0 && first <= 0xEF)
            width = 3;
        else if (first >= 0xF0 && first <= 0xF4)
            width = 4;
        else
            return false;
        if (index + width > text.size()) return false;
        const unsigned char second = static_cast<unsigned char>(text[index + 1]);
        if (second < 0x80 || second > 0xBF || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second > 0x9F) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second > 0x8F))
            return false;
        for (int offset = 2; offset < width; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if (next < 0x80 || next > 0xBF) return false;
        }
        index += static_cast<std::size_t>(width);
    }
    return true;
}

class MaiPdfTool final : public MaiTool {
public:
    explicit MaiPdfTool(MaiPdfRenderer renderer) : mRenderer(std::move(renderer)) {}

    std::string name() const override {
        return "generate_pdf";
    }

    std::string description() const override {
        return "Generate an A4 PDF directly from Markdown content in one call. Use headings, "
               "lists, tables, and inline formatting. Images must be embedded as base64 data "
               "URLs; external resources are unsupported. The output must be a new .pdf file "
               "in a location accessible to this host.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("content":{"type":"string","description":"UTF-8 Markdown to render as the PDF body"},)"
               R"("output_path":{"type":"string","description":"Absolute or relative path for a new .pdf file"}},)"
               R"("required":["content","output_path"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const json args = json::parse(argumentsJson, nullptr, false);
        if (!args.is_object() || !args.contains("output_path") || !args["output_path"].is_string())
            return MaiTool::approvalKeys(argumentsJson, context);
        const std::string path = context.resolvePath(args["output_path"].get<std::string>());
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json arguments = json::parse(argumentsJson, nullptr, false);
        const bool hasContent = arguments.is_object() && arguments.contains("content") &&
                                arguments["content"].is_string();
        const bool hasLegacySource = arguments.is_object() && arguments.contains("source_path") &&
                                     arguments["source_path"].is_string();
        if (!arguments.is_object() || arguments.size() != 2 || hasContent == hasLegacySource ||
            !arguments.contains("output_path") || !arguments["output_path"].is_string()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "generate_pdf requires content and output_path strings");
        }
        const std::string outputName = arguments["output_path"].get<std::string>();
        if (!hasExtension(outputName, ".pdf")) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must end in .pdf");
        }
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "this session has no working directory");
        const std::string output = context.resolvePath(outputName);
        if (output.empty())
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "PDF output must stay inside the area accessible to this host");
        const MaiFilePath outputPath = MaiFilePath::fromUtf8(output);
        if (MaiFileSystem::exists(outputPath))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "PDF output already exists; choose a new filename");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
        std::string html;
        if (hasContent) {
            const std::string content = arguments["content"].get<std::string>();
            if (content.find_first_not_of(" \t\r\n") == std::string::npos ||
                content.size() > kMaxInputBytes || !isValidUtf8(content))
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "Markdown content must be valid UTF-8 and at most 10 MB");
            if (!markdownToHtml(content, html))
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "Markdown could not be rendered or contains an external image");
        } else {
            const std::string sourceName = arguments["source_path"].get<std::string>();
            if (!hasExtension(sourceName, ".html") && !hasExtension(sourceName, ".htm"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "source_path must be an HTML file");
            const std::string source = context.resolvePath(sourceName);
            if (source.empty() || source == output)
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "PDF source must stay inside the area accessible to this host");
            const MaiFilePath sourcePath = MaiFilePath::fromUtf8(source);
            if (!MaiFileSystem::exists(sourcePath) || MaiFileSystem::isDirectory(sourcePath))
                return MaiToolResult::failure(MaiErrorCode::NotFound,
                                              "HTML source file does not exist");
            std::uint64_t sourceBytes = 0;
            if (!MaiFileSystem::fileSize(sourcePath, sourceBytes) || sourceBytes == 0 ||
                sourceBytes > kMaxInputBytes)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "HTML source must be between 1 byte and 10 MB");
            const MaiError readError = MaiFileSystem::readFile(sourcePath, html, kMaxInputBytes);
            if (readError.hasError())
                return MaiToolResult::failure(readError.code(), readError.message());
        }
        if (!isValidUtf8(html))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "PDF content must contain valid UTF-8 text");
        const MaiError directoryError = MaiFileSystem::createDirectories(outputPath.dirName());
        if (directoryError.hasError())
            return MaiToolResult::failure(directoryError.code(), directoryError.message());

        MaiToolResult result;
        try {
            result = mRenderer(html, output, context.cancel);
        } catch (const std::exception& error) {
            if (MaiFileSystem::exists(outputPath)) MaiFileSystem::removeFile(outputPath);
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          std::string("PDF renderer failed: ") + error.what());
        }
        if (result.hasError() || context.isCanceled()) {
            if (MaiFileSystem::exists(outputPath)) MaiFileSystem::removeFile(outputPath);
            if (result.hasError()) return result;
            return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
        }
        std::uint64_t pdfBytes = 0;
        std::string header;
        if (!MaiFileSystem::fileSize(outputPath, pdfBytes) || pdfBytes < 8 ||
            pdfBytes > kMaxPdfBytes || MaiFileSystem::readFile(outputPath, header, 8).hasError() ||
            header.compare(0, 5, "%PDF-") != 0) {
            if (MaiFileSystem::exists(outputPath)) MaiFileSystem::removeFile(outputPath);
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "the PDF renderer did not produce a valid PDF file");
        }
        return MaiToolResult::success(json{
            {"path", outputName},
            {"mime_type", "application/pdf"},
            {"bytes", pdfBytes}}.dump());
    }

private:
    MaiPdfRenderer mRenderer;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiPdfTool(MaiPdfRenderer renderer) {
    if (!renderer) return nullptr;
    return std::make_unique<MaiPdfTool>(std::move(renderer));
}
