#include "MaiAgentSendMediaTool.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

std::string lowerExtension(const std::string& fileName) {
    const std::size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string extension = fileName.substr(dot);
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}

std::string mimeType(const std::string& type, const std::string& extension,
                     const std::string& header) {
    if (type == "image") {
        if (extension == ".png" && header.size() >= 8 &&
            header.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0)
            return "image/png";
        if ((extension == ".jpg" || extension == ".jpeg") && header.size() >= 3 &&
            static_cast<unsigned char>(header[0]) == 0xff &&
            static_cast<unsigned char>(header[1]) == 0xd8 &&
            static_cast<unsigned char>(header[2]) == 0xff)
            return "image/jpeg";
        if (extension == ".gif" && header.size() >= 6 &&
            (header.compare(0, 6, "GIF87a") == 0 || header.compare(0, 6, "GIF89a") == 0))
            return "image/gif";
        if (extension == ".webp" && header.size() >= 12 && header.compare(0, 4, "RIFF") == 0 &&
            header.compare(8, 4, "WEBP") == 0)
            return "image/webp";
        if ((extension == ".heic" || extension == ".heif") && header.size() >= 12 &&
            header.compare(4, 4, "ftyp") == 0)
            return "image/heic";
    } else if (type == "video") {
        if ((extension == ".mp4" || extension == ".mov" || extension == ".m4v") &&
            header.size() >= 12 && header.compare(4, 4, "ftyp") == 0)
            return extension == ".mov" ? "video/quicktime" : "video/mp4";
        if (extension == ".mkv" && header.size() >= 4 &&
            header.compare(0, 4, "\x1a\x45\xdf\xa3", 4) == 0)
            return "video/x-matroska";
    } else if (type == "audio") {
        if (extension == ".mp3" && header.size() >= 3 &&
            (header.compare(0, 3, "ID3") == 0 ||
             (static_cast<unsigned char>(header[0]) == 0xff &&
              (static_cast<unsigned char>(header[1]) & 0xe0) == 0xe0)))
            return "audio/mpeg";
        if (extension == ".wav" && header.size() >= 12 && header.compare(0, 4, "RIFF") == 0 &&
            header.compare(8, 4, "WAVE") == 0)
            return "audio/wav";
        if ((extension == ".m4a" || extension == ".aac") && header.size() >= 12 &&
            header.compare(4, 4, "ftyp") == 0)
            return "audio/mp4";
        if (extension == ".aac" && header.size() >= 2 &&
            static_cast<unsigned char>(header[0]) == 0xff &&
            (static_cast<unsigned char>(header[1]) & 0xf0) == 0xf0)
            return "audio/aac";
        if (extension == ".ogg" && header.size() >= 4 && header.compare(0, 4, "OggS") == 0)
            return "audio/ogg";
        if (extension == ".flac" && header.size() >= 4 && header.compare(0, 4, "fLaC") == 0)
            return "audio/flac";
    }
    return {};
}

class MaiAgentSendMediaTool final : public MaiTool {
public:
    std::string name() const override {
        return "agent_send_media";
    }
    std::string description() const override {
        return "Attach an existing image, video, or audio file from this Agent workspace to the "
               "current AI conversation as a persistent playable media card. This is the way to "
               "deliver a processed media result to the user in the AI chat. It never sends to an "
               "IM contact and does not accept peer_id.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"file_path":{"type":"string"},"type":{"type":"string","enum":["image","video","audio"]},"caption":{"type":"string","maxLength":1000}},"required":["file_path","type"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || request.size() < 2 || request.size() > 3 ||
            !request.contains("file_path") || !request["file_path"].is_string() ||
            !request.contains("type") || !request["type"].is_string() ||
            (request.contains("caption") && !request["caption"].is_string()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_path and type are required; caption is optional");
        for (auto field = request.begin(); field != request.end(); ++field)
            if (field.key() != "file_path" && field.key() != "type" && field.key() != "caption")
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "agent_send_media accepts no peer_id or extra fields");
        const std::string caption = request.value("caption", std::string());
        if (caption.size() > 4000)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "caption must be at most 1000 characters");
        const auto prepared = maiPrepareAgentMedia(request["file_path"].get<std::string>(),
                                                   request["type"].get<std::string>(), context);
        if (!prepared)
            return MaiToolResult::failure(prepared.error().code(), prepared.error().message());
        const MaiAgentMedia& media = prepared.value();
        return MaiToolResult::success(Json{
            {"delivery", "current_ai_session"},
            {"path", media.relativePath},
            {"type", media.type},
            {"mime_type", media.mimeType},
            {"bytes", media.bytes},
            {"caption", caption}}.dump());
    }
};

}  // namespace

MaiResult<MaiAgentMedia> maiPrepareAgentMedia(const std::string& filePath, const std::string& type,
                                              const MaiToolContext& context) {
    if (context.root.empty())
        return {MaiErrorCode::NotConfigured, "Agent workspace is unavailable"};
    if (filePath.empty() || MaiFilePath::fromUtf8(filePath).isAbsolute())
        return {MaiErrorCode::InvalidInput, "file_path must be relative to this Agent workspace"};
    if (type != "image" && type != "video" && type != "audio")
        return {MaiErrorCode::InvalidInput, "type must be image, video, or audio"};
    const std::string resolved = maiResolvePathWithinRoot(context.root, filePath);
    if (resolved.empty())
        return {MaiErrorCode::InvalidInput, "file_path leaves this Agent workspace"};
    const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
    if (!MaiFileSystem::exists(path) || MaiFileSystem::isDirectory(path))
        return {MaiErrorCode::NotFound, "media file does not exist"};
    std::uint64_t size = 0;
    const std::uint64_t limit = type == "video"   ? 2ull * 1024 * 1024 * 1024
                                : type == "audio" ? 200ull * 1024 * 1024
                                                  : 100ull * 1024 * 1024;
    if (!MaiFileSystem::fileSize(path, size) || size == 0 || size > limit)
        return {MaiErrorCode::InvalidInput, "media file is empty or exceeds the size limit"};
    std::string header;
    const MaiError read = MaiFileSystem::readFile(path, header, 32);
    if (read) return {read.code(), read.message()};
    const std::string mime = mimeType(type, lowerExtension(path.baseName().toUtf8()), header);
    if (mime.empty())
        return {MaiErrorCode::InvalidInput, "media file format does not match type or extension"};
    return MaiAgentMedia{filePath, type, mime, size};
}

std::unique_ptr<MaiTool> makeMaiAgentSendMediaTool() {
    return std::make_unique<MaiAgentSendMediaTool>();
}
