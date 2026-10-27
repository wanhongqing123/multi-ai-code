#include "MaiQrCode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <json.hpp>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "qrcodegen.h"

namespace {

using Json = nlohmann::json;

void appendU32(std::string& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>(value >> shift));
}

std::uint32_t crc32(const std::string& bytes, std::size_t begin) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (std::size_t index = begin; index < bytes.size(); ++index) {
        crc ^= static_cast<unsigned char>(bytes[index]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void appendChunk(std::string& png, const char* type, const std::string& body) {
    appendU32(png, static_cast<std::uint32_t>(body.size()));
    const std::size_t begin = png.size();
    png.append(type, 4);
    png.append(body);
    appendU32(png, crc32(png, begin));
}

std::string deflateUncompressed(const std::string& raw) {
    std::string output("\x78\x01", 2);
    std::uint32_t first = 1;
    std::uint32_t second = 0;
    for (unsigned char byte : raw) {
        first = (first + byte) % 65521U;
        second = (second + first) % 65521U;
    }
    for (std::size_t offset = 0; offset < raw.size();) {
        const std::size_t count = (std::min)(std::size_t(65535), raw.size() - offset);
        output.push_back(offset + count == raw.size() ? '\x01' : '\x00');
        output.push_back(static_cast<char>(count));
        output.push_back(static_cast<char>(count >> 8));
        output.push_back(static_cast<char>(~count));
        output.push_back(static_cast<char>((~count) >> 8));
        output.append(raw, offset, count);
        offset += count;
    }
    appendU32(output, (second << 16) | first);
    return output;
}

class MaiQrCodeTool final : public MaiTool {
public:
    std::string name() const override { return "maichat_generate_qr_code"; }
    std::string description() const override {
        return "Generate a scannable QR code PNG from text or a URL in this Agent "
               "Session's workspace. Return the file path and image. The QR code "
               "contains exactly the supplied text; do not put secrets in it.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"text":{"type":"string","minLength":1,"maxLength":768}},"required":["text"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return true; }
    bool requiresPerCallApproval(const std::string&) const override { return true; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "QR code generation was canceled");
        const Json arguments = Json::parse(argumentsJson, nullptr, false);
        if (!arguments.is_object() || arguments.size() != 1 ||
            !arguments.contains("text") || !arguments["text"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "text must be a nonempty UTF-8 string");
        const std::string text = arguments["text"].get<std::string>();
        if (text.empty() || text.size() > 768 || text.find('\0') != std::string::npos)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "text must contain 1 to 768 UTF-8 bytes and no NUL");
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "the Agent workspace is not configured");
        const std::string relative = "generated/qr_" +
            MaiIdGenerator::generate("code_") + ".png";
        const std::string resolved = context.resolvePath(relative);
        if (resolved.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "the QR code output is outside the Agent workspace");
        const MaiFilePath output = MaiFilePath::fromUtf8(resolved);
        const MaiError directory = MaiFileSystem::createDirectories(output.dirName());
        if (directory.hasError())
            return MaiToolResult::failure(directory.code(), directory.message());
        const MaiError saved = maiGenerateQrCodePng(text, output);
        if (saved.hasError())
            return MaiToolResult::failure(saved.code(), saved.message());
        return MaiToolResult::successWithImages(
            Json{{"file_path", output.toGenericUtf8()}, {"format", "png"}}.dump(),
            {MaiToolImage{output.toUtf8(), "image/png"}});
    }
};

}  // namespace

MaiResult<std::string> maiEncodeQrCodePng(const std::string& text) {
    if (text.empty() || text.size() > 768 || text.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput,
                "QR code text must contain 1 to 768 UTF-8 bytes and no NUL"};
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> scratch{};
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> code{};
    if (!qrcodegen_encodeText(text.c_str(), scratch.data(), code.data(),
            qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO, true))
        return {MaiErrorCode::InvalidInput,
                "QR code text is too long for medium error correction"};
    constexpr int border = 4;
    const int modules = qrcodegen_getSize(code.data()) + border * 2;
    const int scale = (std::max)(4, 512 / modules);
    const int width = modules * scale;
    std::string raw;
    raw.reserve(std::size_t(width + 1) * std::size_t(width));
    for (int y = 0; y < width; ++y) {
        raw.push_back('\0');  // PNG filter: none.
        for (int x = 0; x < width; ++x) {
            const bool dark = qrcodegen_getModule(code.data(), x / scale - border,
                                                  y / scale - border);
            raw.push_back(dark ? '\0' : static_cast<char>(255));
        }
    }
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    appendU32(header, std::uint32_t(width));
    appendU32(header, std::uint32_t(width));
    header.append("\x08\x00\x00\x00\x00", 5);  // Grayscale, 8-bit.
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", deflateUncompressed(raw));
    appendChunk(png, "IEND", {});
    return png;
}

MaiError maiGenerateQrCodePng(const std::string& text, const MaiFilePath& output) {
    if (!output.isAbsolute())
        return MaiError::make(MaiErrorCode::InvalidInput,
                              "QR code output path must be absolute");
    const MaiResult<std::string> encoded = maiEncodeQrCodePng(text);
    if (!encoded.isOk()) return encoded.error();
    const MaiFilePath temporary = output.dirName().append(MaiFilePath::fromUtf8(
        MaiIdGenerator::generate("qr_tmp_") + ".png"));
    MaiError error = MaiFileSystem::createEmptyFile(temporary);
    if (error.hasError()) return error;
    error = MaiFileSystem::writeFile(temporary, encoded.value());
    if (!error.hasError()) error = MaiFileSystem::publishNewFile(temporary, output);
    if (error.hasError()) MaiFileSystem::removeFile(temporary);
    return error;
}

void registerMaiQrCodeTools(MaiToolRegistry& tools) {
    tools.add(std::make_unique<MaiQrCodeTool>());
}
