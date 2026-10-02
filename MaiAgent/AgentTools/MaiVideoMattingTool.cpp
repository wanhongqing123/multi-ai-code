#include "MaiVideoMattingTool.h"

#include <cmath>
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

bool parseColor(const std::string& text, std::array<std::uint8_t, 3>& rgb) {
    if (text.size() != 7 || text[0] != '#') return false;
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
        const int high = hex(text[channel * 2 + 1]);
        const int low = hex(text[channel * 2 + 2]);
        if (high < 0 || low < 0) return false;
        rgb[channel] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

class MaiVideoMattingTool final : public MaiTool {
public:
    MaiVideoMattingTool(MaiVideoMattingProcessor processor, std::string modelPath,
                        std::string runtimePath, const void* apiBase)
        : mProcessor(processor),
          mModelPath(std::move(modelPath)),
          mRuntimePath(std::move(runtimePath)),
          mApiBase(apiBase) {}

    std::string name() const override {
        return "cv_video_matting";
    }
    std::string description() const override {
        return "Replace a person's video background on-device with a solid color, another image, "
               "or a blurred version of the original. Stream-process frames through recurrent "
               "RVM ONNX inference, keep opaque source colors, and copy original audio without "
               "re-encoding. Create a new MP4/MOV/MKV file without changing the source.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"input_path":{"type":"string"},"output_path":{"type":"string"},"background":{"type":"object","properties":{"mode":{"type":"string","enum":["solid","image","blur"]},"color":{"type":"string","pattern":"^#[0-9A-Fa-f]{6}$"},"image_path":{"type":"string"},"blur_sigma":{"type":"number","minimum":1,"maximum":60}},"required":["mode"],"additionalProperties":false},"downsample_ratio":{"type":"number","exclusiveMinimum":0,"maximum":1}},"required":["input_path","output_path","background"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.contains("input_path") ||
            !request["input_path"].is_string() || !request.contains("output_path") ||
            !request["output_path"].is_string() || !request.contains("background") ||
            !request["background"].is_object())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "input_path, output_path, and background are required");
        const std::string input = context.resolvePath(request["input_path"].get<std::string>());
        if (input.empty() || !MaiFileSystem::exists(MaiFilePath::fromUtf8(input)) ||
            MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(input)))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "input video must be an accessible local file");
        const std::string suppliedOutput = request["output_path"].get<std::string>();
        if (suppliedOutput.empty() || MaiFilePath::fromUtf8(suppliedOutput).isAbsolute())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must be relative to the Agent workspace");
        const std::string output = maiResolvePathWithinRoot(context.root, suppliedOutput);
        if (output.empty() || output == input ||
            MaiFileSystem::exists(MaiFilePath::fromUtf8(output)))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "output_path must name a new file inside the Agent workspace");
        const std::string suffix = MaiFilePath::fromUtf8(output).baseName().toUtf8();
        if (suffix.size() < 4 || (suffix.compare(suffix.size() - 4, 4, ".mp4") != 0 &&
                                  suffix.compare(suffix.size() - 4, 4, ".mov") != 0 &&
                                  suffix.compare(suffix.size() - 4, 4, ".mkv") != 0))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must end in .mp4, .mov, or .mkv");
        const Json& background = request["background"];
        if (!background.contains("mode") || !background["mode"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "background.mode is required");
        MaiVideoMattingOptions options;
        options.modelPath = mModelPath;
        options.runtimePath = mRuntimePath;
        options.ortApiBase = mApiBase;
        const std::string mode = background["mode"].get<std::string>();
        if (mode == "solid") {
            if (!background.contains("color") || !background["color"].is_string() ||
                !parseColor(background["color"].get<std::string>(), options.solidRgb))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "solid background needs color #RRGGBB");
        } else if (mode == "image") {
            if (!background.contains("image_path") || !background["image_path"].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "image background needs image_path");
            options.backgroundKind = MaiMattingBackgroundKind::Image;
            options.backgroundImagePath =
                context.resolvePath(background["image_path"].get<std::string>());
            if (options.backgroundImagePath.empty() ||
                !MaiFileSystem::exists(MaiFilePath::fromUtf8(options.backgroundImagePath)))
                return MaiToolResult::failure(MaiErrorCode::NotFound,
                                              "background image is not accessible");
        } else if (mode == "blur") {
            options.backgroundKind = MaiMattingBackgroundKind::Blur;
            if (background.contains("blur_sigma")) {
                if (!background["blur_sigma"].is_number())
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "blur_sigma must be a number from 1 to 60");
                options.blurSigma = background["blur_sigma"].get<double>();
            }
            if (!std::isfinite(options.blurSigma) || options.blurSigma < 1 ||
                options.blurSigma > 60)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "blur_sigma must be a number from 1 to 60");
        } else {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "background.mode must be solid, image, or blur");
        }
        if (request.contains("downsample_ratio")) {
            if (!request["downsample_ratio"].is_number())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "downsample_ratio must be between 0 and 1");
            options.downsampleRatio = request["downsample_ratio"].get<float>();
        }
        if (!std::isfinite(options.downsampleRatio) || options.downsampleRatio <= 0 ||
            options.downsampleRatio > 1)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "downsample_ratio must be between 0 and 1");
        const MaiError directory =
            MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(output).dirName());
        if (directory) return MaiToolResult::failure(directory.code(), directory.message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "video matting was canceled");
        const MaiVideoMattingResult processed = mProcessor(input, output, options, context);
        if (context.isCanceled() || processed.error == "canceled")
            return MaiToolResult::failure(MaiErrorCode::Canceled, "video matting was canceled");
        if (!processed.error.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal, processed.error);
        return MaiToolResult::success(Json{{"path", processed.outputPath},
                                           {"frames", processed.frames},
                                           {"duration_s", processed.durationSeconds},
                                           {"method", "rvm_mobilenetv3_onnx"},
                                           {"audio_streams_copied", processed.audioStreamsCopied}}
                                          .dump());
    }

private:
    MaiVideoMattingProcessor mProcessor;
    std::string mModelPath;
    std::string mRuntimePath;
    const void* mApiBase = nullptr;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiVideoMattingTool(MaiVideoMattingProcessor processor,
                                                 std::string modelPath, std::string runtimePath,
                                                 const void* apiBase) {
    if (processor == nullptr || modelPath.empty()) return nullptr;
    return std::make_unique<MaiVideoMattingTool>(processor, std::move(modelPath),
                                                 std::move(runtimePath), apiBase);
}
