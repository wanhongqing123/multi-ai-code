#include "MaiCvVideoTools.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

class MaiCvVideoTool final : public MaiTool {
public:
    MaiCvVideoTool(MaiCvVideoAnalysisKind kind, MaiCvVideoAnalyzer analyzer)
        : mKind(kind), mAnalyzer(analyzer) {}

    std::string name() const override {
        return mKind == MaiCvVideoAnalysisKind::Scene ? "cv_scene_detect" : "cv_motion_detect";
    }

    std::string description() const override {
        // 场景切分看镜头变化，运动检测看帧差；两者只返回时间片段，
        // 不理解剧情，也不识别人脸或人物身份，源视频保持不变。
        if (mKind == MaiCvVideoAnalysisKind::Scene)
            return "Stream-decode a local video and detect visual shot changes with OpenCV. "
                   "Return ordered start/end time segments in seconds. This is visual change "
                   "analysis, not semantic understanding. The input file is unchanged.";
        return "Stream-decode a local video and detect periods of visible motion using OpenCV "
               "frame differences. Return start/end time segments in seconds. Motion can be "
               "caused by people, objects, camera movement, or lighting; this tool does not "
               "identify people. The input file is unchanged.";
    }

    std::string parametersSchema() const override {
        // 两种分析共用本地视频 path；采样间隔和阈值控制速度/灵敏度，
        // max_segments 限制输出大小，不能把结果当成语义识别。
        if (mKind == MaiCvVideoAnalysisKind::Scene)
            return R"({"type":"object","properties":{"path":{"type":"string"},"sample_interval_s":{"type":"number","minimum":0.1,"maximum":5},"threshold":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1},"min_scene_s":{"type":"number","minimum":0,"maximum":30},"max_segments":{"type":"integer","minimum":1,"maximum":2000}},"required":["path"],"additionalProperties":false})";
        return R"({"type":"object","properties":{"path":{"type":"string"},"sample_interval_s":{"type":"number","minimum":0.1,"maximum":5},"motion_ratio_threshold":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1},"pixel_threshold":{"type":"integer","minimum":1,"maximum":255},"min_motion_s":{"type":"number","minimum":0,"maximum":30},"max_segments":{"type":"integer","minimum":1,"maximum":2000}},"required":["path"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (!mAnalyzer)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "OpenCV video analysis is unavailable on this host");
        const Json request = Json::parse(argumentsJson, nullptr, false);
        if (request.is_discarded() || !request.is_object() || !request.contains("path") ||
            !request["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "video analysis requires one local video path");
        const std::string path = context.resolvePath(request["path"].get<std::string>());
        if (path.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "video path is outside the accessible area");
        const MaiFilePath filePath = MaiFilePath::fromUtf8(path);
        if (!MaiFileSystem::exists(filePath) || MaiFileSystem::isDirectory(filePath))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "video file does not exist: " + path);

        MaiCvVideoAnalysisOptions options;
        options.kind = mKind;
        options.sampleIntervalSeconds = mKind == MaiCvVideoAnalysisKind::Scene ? 0.5 : 0.25;
        options.threshold = mKind == MaiCvVideoAnalysisKind::Scene ? 0.45 : 0.015;
        options.minimumSegmentSeconds = mKind == MaiCvVideoAnalysisKind::Scene ? 0.75 : 0.5;
        int maxSegments = 500;
        const char* limitKey =
            mKind == MaiCvVideoAnalysisKind::Scene ? "threshold" : "motion_ratio_threshold";
        const char* minimumKey =
            mKind == MaiCvVideoAnalysisKind::Scene ? "min_scene_s" : "min_motion_s";
        if (!number(request, "sample_interval_s", options.sampleIntervalSeconds, 0.1, 5) ||
            !number(request, limitKey, options.threshold, 0, 1, true) ||
            !number(request, minimumKey, options.minimumSegmentSeconds, 0, 30) ||
            !integer(request, "pixel_threshold", options.pixelThreshold, 1, 255) ||
            !integer(request, "max_segments", maxSegments, 1, 2000)) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "invalid video analysis parameter range");
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "video analysis was canceled");
        const MaiCvVideoAnalysisResult analyzed = mAnalyzer(path, options, context);
        if (context.isCanceled() || analyzed.error == "canceled")
            return MaiToolResult::failure(MaiErrorCode::Canceled, "video analysis was canceled");
        if (!analyzed.error.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, analyzed.error);

        const std::size_t count =
            std::min(analyzed.segments.size(), static_cast<std::size_t>(maxSegments));
        const auto roundedSeconds = [](double seconds) {
            return std::round(seconds * 1000.0) / 1000.0;
        };
        Json segments = Json::array();
        for (std::size_t index = 0; index < count; ++index) {
            const MaiCvTimeSegment& segment = analyzed.segments[index];
            segments.push_back({{"start_s", roundedSeconds(segment.startSeconds)},
                                {"end_s", roundedSeconds(segment.endSeconds)}});
        }
        const bool truncated = count < analyzed.segments.size();
        const Json output = {
            {"segments", std::move(segments)},
            {"duration_s", roundedSeconds(analyzed.durationSeconds)},
            {"sampled_frames", analyzed.sampledFrames},
            {"decoded_frames", analyzed.decodedFrames},
            {"sample_interval_s", options.sampleIntervalSeconds},
            {"threshold", options.threshold},
            {"total_segments", analyzed.segments.size()},
            {"truncated", truncated},
            {"method", mKind == MaiCvVideoAnalysisKind::Scene ? "opencv_hsv_histogram"
                                                              : "opencv_frame_difference"}};
        return MaiToolResult::success(output.dump(), truncated);
    }

private:
    static bool number(const Json& request, const char* key, double& value, double minimum,
                       double maximum, bool exclusive = false) {
        if (!request.contains(key)) return true;
        if (!request[key].is_number()) return false;
        value = request[key].get<double>();
        return std::isfinite(value) && (exclusive ? value > minimum && value < maximum
                                                  : value >= minimum && value <= maximum);
    }

    static bool integer(const Json& request, const char* key, int& value, int minimum,
                        int maximum) {
        if (!request.contains(key)) return true;
        if (!request[key].is_number_integer()) return false;
        const std::int64_t supplied = request[key].get<std::int64_t>();
        if (supplied < minimum || supplied > maximum) return false;
        value = static_cast<int>(supplied);
        return true;
    }

    MaiCvVideoAnalysisKind mKind;
    MaiCvVideoAnalyzer mAnalyzer;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCvSceneDetectTool(MaiCvVideoAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvVideoTool>(MaiCvVideoAnalysisKind::Scene, analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvMotionDetectTool(MaiCvVideoAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvVideoTool>(MaiCvVideoAnalysisKind::Motion, analyzer)
                    : nullptr;
}
