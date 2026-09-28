#include "MaiPdfTool.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kMaxHtmlBytes = 10u * 1024 * 1024;
constexpr std::uint64_t kMaxPdfBytes = 100u * 1024 * 1024;

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
        return "Generate an A4 PDF from a self-contained HTML file in the working directory. "
               "Write the HTML first; inline any images as data URLs. External resources are "
               "unsupported. The output must be a new .pdf "
               "file inside the working directory.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("source_path":{"type":"string","description":"Existing .html or .htm file inside the working directory"},)"
               R"("output_path":{"type":"string","description":"New .pdf path inside the working directory"}},)"
               R"("required":["source_path","output_path"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json arguments = json::parse(argumentsJson, nullptr, false);
        if (!arguments.is_object() || arguments.size() != 2 || !arguments.contains("source_path") ||
            !arguments["source_path"].is_string() || !arguments.contains("output_path") ||
            !arguments["output_path"].is_string()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "generate_pdf requires source_path and output_path strings");
        }
        const std::string sourceName = arguments["source_path"].get<std::string>();
        const std::string outputName = arguments["output_path"].get<std::string>();
        if ((!hasExtension(sourceName, ".html") && !hasExtension(sourceName, ".htm")) ||
            !hasExtension(outputName, ".pdf")) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "source_path must be HTML and output_path must end in .pdf");
        }
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "this session has no working directory");
        const std::string source = maiResolvePathWithinRoot(context.root, sourceName);
        const std::string output = maiResolvePathWithinRoot(context.root, outputName);
        if (source.empty() || output.empty())
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "PDF source and output must stay inside the working directory");
        if (source == output)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "PDF output must differ from its HTML source");
        const MaiFilePath sourcePath = MaiFilePath::fromUtf8(source);
        const MaiFilePath outputPath = MaiFilePath::fromUtf8(output);
        if (!MaiFileSystem::exists(sourcePath) || MaiFileSystem::isDirectory(sourcePath))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "HTML source file does not exist");
        if (MaiFileSystem::exists(outputPath))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "PDF output already exists; choose a new filename");
        std::uint64_t sourceBytes = 0;
        if (!MaiFileSystem::fileSize(sourcePath, sourceBytes) || sourceBytes == 0 ||
            sourceBytes > kMaxHtmlBytes)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "HTML source must be between 1 byte and 10 MB");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
        std::string html;
        const MaiError readError = MaiFileSystem::readFile(sourcePath, html, kMaxHtmlBytes);
        if (readError.hasError())
            return MaiToolResult::failure(readError.code(), readError.message());
        if (!isValidUtf8(html))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "HTML source must contain valid UTF-8 text");
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
        return MaiToolResult::success("Created " + outputName + " (" + std::to_string(pdfBytes) +
                                      " bytes).");
    }

private:
    MaiPdfRenderer mRenderer;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiPdfTool(MaiPdfRenderer renderer) {
    if (!renderer) return nullptr;
    return std::make_unique<MaiPdfTool>(std::move(renderer));
}
