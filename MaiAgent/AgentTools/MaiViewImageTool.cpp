#include "MaiViewImageTool.h"

#include <cstddef>
#include <memory>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

using json = nlohmann::json;

namespace {

constexpr std::uint64_t kMaximumImageBytes = 20u * 1024u * 1024u;

std::string detectMimeType(const std::string& bytes) {
    if (bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFFu &&
        static_cast<unsigned char>(bytes[1]) == 0xD8u &&
        static_cast<unsigned char>(bytes[2]) == 0xFFu)
        return "image/jpeg";
    if (bytes.size() >= 6 &&
        (bytes.compare(0, 6, "GIF87a") == 0 || bytes.compare(0, 6, "GIF89a") == 0))
        return "image/gif";
    if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    return {};
}

class MaiViewImageTool final : public MaiTool {
public:
    std::string name() const override {
        return "view_image";
    }

    std::string description() const override {
        return "View an existing PNG, JPEG, WebP, or GIF file inside the working directory. "
               "Use this when visual inspection is needed for an image already on disk.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"Image path inside the working directory"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
        if (arguments.is_discarded() || !arguments.is_object() || arguments.size() != 1 ||
            !arguments.contains("path") || !arguments["path"].is_string()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "view_image requires one string parameter named path");
        }
        if (context.model == "glm-5.3") {
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "The current glm-5.3 model accepts text only. Ask the user to switch to "
                "glm-5.3-flash before viewing an image.");
        }

        const std::string rawPath = arguments["path"].get<std::string>();
        const std::string resolved = maiResolvePathWithinRoot(context.root, rawPath);
        if (resolved.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "Image path is outside the working directory and was rejected: " + rawPath);
        }
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
        if (!MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "image file does not exist: " + rawPath);
        if (MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image path points to a directory: " + rawPath);

        std::string bytes;
        bool truncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(path, bytes, kMaximumImageBytes, &truncated);
        if (readError.hasError())
            return MaiToolResult::failure(readError.code(), readError.message());
        if (truncated) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image exceeds the 20 MB input limit: " + rawPath);
        }
        const std::string mimeType = detectMimeType(bytes);
        if (mimeType.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "file is not a supported PNG, JPEG, WebP, or GIF image: " + rawPath);
        }

        return MaiToolResult::successWithImages("Loaded the image from " + rawPath + ".",
                                                {MaiToolImage{path.toUtf8(), mimeType}});
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiViewImageTool() {
    return std::make_unique<MaiViewImageTool>();
}
