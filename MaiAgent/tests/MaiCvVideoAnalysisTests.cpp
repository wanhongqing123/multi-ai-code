#include "MaiCvVideoAnalysis.h"
#include "MaiCvVideoTools.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiTool.h"

int main(int argc, char** argv) {
    const bool reportOnly = argc == 3 && std::string(argv[1]) == "--report";
    if (argc != 2 && !reportOnly) return 2;
    const std::string path = reportOnly ? argv[2] : argv[1];
    MaiToolContext context;

    MaiCvVideoAnalysisOptions sceneOptions;
    sceneOptions.sampleIntervalSeconds = 0.25;
    sceneOptions.threshold = 0.4;
    const MaiCvVideoAnalysisResult scenes = analyzeMaiCvVideo(path, sceneOptions, context);
    if (!scenes.error.empty() ||
        (!reportOnly && (std::abs(scenes.durationSeconds - 4.0) > 0.1 ||
                         scenes.sampledFrames < 12 || scenes.segments.size() < 2 ||
                         std::abs(scenes.segments.front().endSeconds - 1.0) > 0.3))) {
        std::printf("scene analysis failed: %s, segments=%zu, duration=%.2f\n",
                    scenes.error.c_str(), scenes.segments.size(), scenes.durationSeconds);
        return 3;
    }

    MaiCvVideoAnalysisOptions motionOptions;
    motionOptions.kind = MaiCvVideoAnalysisKind::Motion;
    motionOptions.sampleIntervalSeconds = 0.125;
    motionOptions.threshold = 0.02;
    motionOptions.minimumSegmentSeconds = 0.25;
    const MaiCvVideoAnalysisResult motion = analyzeMaiCvVideo(path, motionOptions, context);
    bool hasLateMotion = false;
    for (const MaiCvTimeSegment& segment : motion.segments) {
        if (segment.endSeconds >= 3.0 && segment.startSeconds <= 3.0) hasLateMotion = true;
    }
    if (!motion.error.empty() || (!reportOnly && !hasLateMotion)) {
        std::printf("motion analysis failed: %s, segments=%zu\n", motion.error.c_str(),
                    motion.segments.size());
        return 4;
    }

    context.root = MaiFilePath::fromUtf8(path).dirName().toUtf8();
    const std::string name = MaiFilePath::fromUtf8(path).baseName().toUtf8();
    auto sceneTool = makeMaiCvSceneDetectTool(analyzeMaiCvVideo);
    auto motionTool = makeMaiCvMotionDetectTool(analyzeMaiCvVideo);
    const nlohmann::json input = {{"path", name}, {"sample_interval_s", 0.25}};
    const MaiToolResult sceneOutput = sceneTool->execute(input.dump(), context);
    const MaiToolResult motionOutput = motionTool->execute(input.dump(), context);
    if (sceneOutput.hasError() || motionOutput.hasError()) return 7;
    const nlohmann::json sceneData = nlohmann::json::parse(sceneOutput.output());
    const nlohmann::json motionData = nlohmann::json::parse(motionOutput.output());
    if (!reportOnly && (sceneData["segments"].size() < 2 || motionData["segments"].empty()))
        return 8;
    std::printf("cv_scene_detect: %s\n", sceneOutput.output().c_str());
    std::printf("cv_motion_detect: %s\n", motionOutput.output().c_str());

    if (reportOnly) return 0;

    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    const MaiCvVideoAnalysisResult stopped = analyzeMaiCvVideo(path, sceneOptions, context);
    if (stopped.error != "canceled") return 5;
    context.cancel = nullptr;
    if (analyzeMaiCvVideo(path + ".missing", sceneOptions, context).error.empty()) return 6;
    return 0;
}
