#include "MaiFaceBeautifyTool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

struct Levels {
    float smooth;
    float whiten;
    float slimFace;
    float enlargeEye;
};

Levels presetLevels(const std::string& name) {
    if (name == "natural") return {0.28f, 0.10f, 0.10f, 0.08f};
    if (name == "fresh") return {0.42f, 0.20f, 0.17f, 0.14f};
    if (name == "refined") return {0.57f, 0.28f, 0.25f, 0.22f};
    return {};
}

bool readLevel(const Json& effects, const char* name, float& value) {
    if (!effects.contains(name)) return true;
    if (!effects[name].is_number()) return false;
    const double parsed = effects[name].get<double>();
    if (!std::isfinite(parsed) || parsed < 0 || parsed > 1) return false;
    value = static_cast<float>(parsed);
    return true;
}

std::string generatedOutputName() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "beautified-" + std::to_string(now) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".png";
}

class MaiFaceBeautifyTool final : public MaiTool {
public:
    MaiFaceBeautifyTool(MaiFaceBeautifyProcessor processor, std::string detectorModelPath,
                        std::string landmarkModelPath, std::string runtimePath,
                        const void* ortApiBase, MaiFaceImageDecoder decodeImage,
                        MaiFacePngEncoder encodePng)
        : mProcessor(processor),
          mDetectorModelPath(std::move(detectorModelPath)),
          mLandmarkModelPath(std::move(landmarkModelPath)),
          mRuntimePath(std::move(runtimePath)),
          mOrtApiBase(ortApiBase),
          mDecodeImage(decodeImage),
          mEncodePng(encodePng) {}

    std::string name() const override {
        return "mobile_beautify_face";
    }
    std::string description() const override {
        return "Beautify faces in one local image with skin smoothing, selective brightening, "
               "face slimming, and eye enlargement. Creates a new PNG without modifying the "
               "source. Works on mobile and desktop. Use preset or effects; both may be combined.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"image":{"type":"string"},"output_path":{"type":"string"},"effects":{"type":"object","properties":{"smooth":{"type":"number","minimum":0,"maximum":1},"whiten":{"type":"number","minimum":0,"maximum":1},"slimFace":{"type":"number","minimum":0,"maximum":1},"enlargeEye":{"type":"number","minimum":0,"maximum":1}},"additionalProperties":false},"preset":{"type":"string","enum":["natural","fresh","refined"]},"strength":{"type":"number","minimum":0,"maximum":1}},"required":["image"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.contains("image") || !request["image"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "image path is required");
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "Agent workspace is not configured");
        const std::string input = context.resolvePath(request["image"].get<std::string>());
        if (input.empty() || !MaiFileSystem::exists(MaiFilePath::fromUtf8(input)) ||
            MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(input)))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "image must be an accessible local file");

        if (request.contains("output_path") && !request["output_path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must be a relative PNG path");
        const std::string requestedOutput = request.contains("output_path")
                                                ? request["output_path"].get<std::string>()
                                                : generatedOutputName();
        if (requestedOutput.empty() || MaiFilePath::fromUtf8(requestedOutput).isAbsolute())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must be relative to the Agent workspace");
        const std::string output = maiResolvePathWithinRoot(context.root, requestedOutput);
        const std::string outputName = MaiFilePath::fromUtf8(output).baseName().toUtf8();
        if (output.empty() || output == input ||
            MaiFileSystem::exists(MaiFilePath::fromUtf8(output)) || outputName.size() < 4 ||
            outputName.compare(outputName.size() - 4, 4, ".png") != 0)
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "output_path must be a new .png inside the Agent workspace");
        if (!request.contains("preset") && !request.contains("effects"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "preset or effects is required");
        std::string preset;
        if (request.contains("preset")) {
            if (!request["preset"].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid preset");
            preset = request["preset"].get<std::string>();
            if (preset != "natural" && preset != "fresh" && preset != "refined")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid preset");
        }
        const Json effects = request.contains("effects") ? request["effects"] : Json::object();
        if (!effects.is_object())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "effects must be an object");
        Levels extra{};
        if (!readLevel(effects, "smooth", extra.smooth) ||
            !readLevel(effects, "whiten", extra.whiten) ||
            !readLevel(effects, "slimFace", extra.slimFace) ||
            !readLevel(effects, "enlargeEye", extra.enlargeEye))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "each effect must be a number from 0 to 1");
        double strength = 1;
        if (request.contains("strength")) {
            if (!request["strength"].is_number())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid strength");
            strength = request["strength"].get<double>();
        }
        if (!std::isfinite(strength) || strength < 0 || strength > 1)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "strength must be from 0 to 1");
        const Levels base = presetLevels(preset);
        MaiFaceBeautifyOptions options;
        const auto combine = [strength](float a, float b) {
            return static_cast<float>(strength) * std::min(1.0f, a + b);
        };
        options.smooth = combine(base.smooth, extra.smooth);
        options.whiten = combine(base.whiten, extra.whiten);
        options.slimFace = combine(base.slimFace, extra.slimFace);
        options.enlargeEye = combine(base.enlargeEye, extra.enlargeEye);
        options.detectorModelPath = mDetectorModelPath;
        options.landmarkModelPath = mLandmarkModelPath;
        options.runtimePath = mRuntimePath;
        options.ortApiBase = mOrtApiBase;
        options.decodeImage = mDecodeImage;
        options.encodePng = mEncodePng;
        const MaiError directory =
            MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(output).dirName());
        if (directory) return MaiToolResult::failure(directory.code(), directory.message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "face beautification was canceled");
        const MaiError reserve = MaiFileSystem::createEmptyFile(MaiFilePath::fromUtf8(output));
        if (reserve) return MaiToolResult::failure(reserve.code(), reserve.message());
        const MaiFaceBeautifyResult result = mProcessor(input, output, options, context);
        if (!result.errorCode.empty()) {
            MaiFileSystem::removeFile(MaiFilePath::fromUtf8(output));
            const MaiErrorCode code = result.errorCode == "face_not_detected"
                                          ? MaiErrorCode::NotFound
                                          : MaiErrorCode::Internal;
            return MaiToolResult::failure(
                code, Json{{"code", result.errorCode}, {"message", result.errorMessage}}.dump());
        }
        return MaiToolResult::success(Json{
            {"path", result.outputPath},
            {"mime_type", "image/png"},
            {"faces", result.faceCount},
            {"preset", preset}}.dump());
    }

private:
    MaiFaceBeautifyProcessor mProcessor;
    std::string mDetectorModelPath;
    std::string mLandmarkModelPath;
    std::string mRuntimePath;
    const void* mOrtApiBase = nullptr;
    MaiFaceImageDecoder mDecodeImage = nullptr;
    MaiFacePngEncoder mEncodePng = nullptr;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiFaceBeautifyTool(MaiFaceBeautifyProcessor processor,
                                                 std::string detectorModelPath,
                                                 std::string landmarkModelPath,
                                                 std::string runtimePath, const void* ortApiBase,
                                                 MaiFaceImageDecoder decodeImage,
                                                 MaiFacePngEncoder encodePng) {
    if (!processor || detectorModelPath.empty() || landmarkModelPath.empty()) return nullptr;
    return std::make_unique<MaiFaceBeautifyTool>(
        processor, std::move(detectorModelPath), std::move(landmarkModelPath),
        std::move(runtimePath), ortApiBase, decodeImage, encodePng);
}
