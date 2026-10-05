#include "MaiZlibTool.h"

#include <zlib.h>

#include <json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumInputBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumOutputBytes = 64 * 1024 * 1024;

int windowBits(const std::string& format, bool compress) {
    if (format == "gzip") return MAX_WBITS + 16;
    if (format == "zlib") return MAX_WBITS;
    if (format == "deflate") return -MAX_WBITS;
    if (!compress && format == "auto") return MAX_WBITS + 32;
    return 0;
}

std::string defaultOutputPath(const std::string& input, bool compress) {
    if (compress) return input + ".gz";
    if (input.size() > 3 && input.compare(input.size() - 3, 3, ".gz") == 0)
        return input.substr(0, input.size() - 3);
    return input + ".uncompressed";
}

class ZlibTool final : public MaiTool {
public:
    explicit ZlibTool(bool compress) : mCompress(compress) {}

    std::string name() const override {
        return mCompress ? "zlib_compress" : "zlib_decompress";
    }
    std::string description() const override {
        return mCompress
                   ? "Compress a workspace file to a new gzip, zlib, or raw deflate file. "
                     "The source is unchanged; input limit 16 MB."
                   : "Decompress a gzip or zlib workspace file to a new file. Use format=auto "
                     "to detect gzip/zlib, or deflate for a raw stream. Output limit 64 MB; "
                     "the source is unchanged.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"input_path":{"type":"string"},"output_path":{"type":"string"},"format":{"type":"string","enum":["gzip","zlib","deflate","auto"]}},"required":["input_path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::string approvalKey(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("input_path", Json{}).is_string() ||
            (args.contains("output_path") && !args["output_path"].is_string()))
            return name() + ":<invalid>";
        const std::string input = args["input_path"].get<std::string>();
        const std::string output = args.value("output_path", std::string{});
        return name() + ":" + (output.empty() ? defaultOutputPath(input, mCompress) : output);
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("input_path", Json{}).is_string() ||
            (args.contains("output_path") && !args["output_path"].is_string()) ||
            (args.contains("format") && !args["format"].is_string()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "expected input_path and optional string parameters");
        const std::string inputPath = args["input_path"].get<std::string>();
        std::string outputPath = args.value("output_path", std::string{});
        if (outputPath.empty()) outputPath = defaultOutputPath(inputPath, mCompress);
        const std::string format = args.value("format", mCompress ? "gzip" : "auto");
        if (windowBits(format, mCompress) == 0)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "unsupported zlib stream format");
        const std::string input = context.resolvePath(inputPath);
        const std::string output = context.resolvePath(outputPath);
        if (input.empty() || output.empty() || input == output)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "input and output must be distinct accessible paths");
        const MaiFilePath source = MaiFilePath::fromUtf8(input);
        const MaiFilePath target = MaiFilePath::fromUtf8(output);
        std::uint64_t size = 0;
        if (!MaiFileSystem::fileSize(source, size) || size > kMaximumInputBytes)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "input file is missing or exceeds 16 MB");
        if (MaiFileSystem::exists(target))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output file already exists; choose a new path");
        std::string bytes;
        const MaiError read = MaiFileSystem::readFile(source, bytes, kMaximumInputBytes);
        if (read.hasError()) return MaiToolResult::failure(read.code(), read.message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "compression was canceled");
        auto transformed = maiTransformZlibBytes(bytes, mCompress, format, kMaximumOutputBytes);
        if (!transformed)
            return MaiToolResult::failure(transformed.error().code(),
                                          transformed.error().message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "compression was canceled");
        const MaiError created = MaiFileSystem::createEmptyFile(target);
        if (created.hasError()) return MaiToolResult::failure(created.code(), created.message());
        const MaiError written = MaiFileSystem::writeFile(target, transformed.value());
        if (written.hasError()) {
            MaiFileSystem::removeFile(target);
            return MaiToolResult::failure(written.code(), written.message());
        }
        return MaiToolResult::success(Json{
            {"input_path", inputPath},
            {"output_path", outputPath},
            {"format", format},
            {"input_bytes", bytes.size()},
            {"output_bytes",
             transformed.value().size()}}.dump());
    }

private:
    bool mCompress;
};

}  // namespace

MaiResult<std::string> maiTransformZlibBytes(const std::string& input, bool compress,
                                             const std::string& format,
                                             std::size_t maxOutputBytes) {
    const int bits = windowBits(format, compress);
    if (bits == 0 || input.size() > static_cast<std::size_t>(UINT_MAX) || maxOutputBytes == 0)
        return {MaiErrorCode::InvalidInput, "invalid zlib format or size limit"};
    z_stream stream{};
    const int initialized = compress ? deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                                    bits, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY)
                                     : inflateInit2(&stream, bits);
    if (initialized != Z_OK) return {MaiErrorCode::Internal, "zlib initialization failed"};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    std::string output;
    std::array<unsigned char, 32 * 1024> chunk{};
    while (true) {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<uInt>(chunk.size());
        const int status = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = chunk.size() - stream.avail_out;
        if (produced > maxOutputBytes - output.size()) {
            if (compress)
                deflateEnd(&stream);
            else
                inflateEnd(&stream);
            return {MaiErrorCode::InvalidInput, "zlib output exceeds configured size limit"};
        }
        output.append(reinterpret_cast<const char*>(chunk.data()), produced);
        if (status == Z_STREAM_END) break;
        if (status != Z_OK || (!compress && produced == 0 && stream.avail_in == 0)) {
            if (compress)
                deflateEnd(&stream);
            else
                inflateEnd(&stream);
            return {MaiErrorCode::InvalidInput, "invalid or incomplete zlib stream"};
        }
    }
    const bool trailing = stream.avail_in != 0;
    if (compress)
        deflateEnd(&stream);
    else
        inflateEnd(&stream);
    if (trailing) return {MaiErrorCode::InvalidInput, "zlib stream has trailing bytes"};
    return output;
}

std::unique_ptr<MaiTool> makeMaiZlibCompressTool() {
    return std::make_unique<ZlibTool>(true);
}

std::unique_ptr<MaiTool> makeMaiZlibDecompressTool() {
    return std::make_unique<ZlibTool>(false);
}
