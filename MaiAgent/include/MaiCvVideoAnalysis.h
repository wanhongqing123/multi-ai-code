#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MaiToolContext;

enum class MaiCvVideoAnalysisKind { Scene, Motion };

struct MaiCvVideoAnalysisOptions {
    MaiCvVideoAnalysisKind kind = MaiCvVideoAnalysisKind::Scene;
    double sampleIntervalSeconds = 0.5;
    double threshold = 0.45;
    int pixelThreshold = 20;
    double minimumSegmentSeconds = 0.75;
};

struct MaiCvTimeSegment {
    double startSeconds = 0;
    double endSeconds = 0;
};

struct MaiCvVideoAnalysisResult {
    std::vector<MaiCvTimeSegment> segments;
    double durationSeconds = 0;
    std::uint64_t sampledFrames = 0;
    std::uint64_t decodedFrames = 0;
    std::string error;
};

// Stream-decode one local video and analyze sampled frames with OpenCV.
// The caller must resolve and validate the input path before calling. No file
// is created or modified. This call may take as long as decoding the source;
// run it on a worker thread. A canceled call returns an error and no segments.
// All returned times are seconds relative to the beginning of the media.
// sampleIntervalSeconds must be 0.1..5, threshold strictly between 0 and 1,
// pixelThreshold 1..255, and minimumSegmentSeconds 0..30. Invalid input and
// media decode failures return a nonempty error. An empty segment list is valid
// for motion analysis when no qualifying motion occurs.
MaiCvVideoAnalysisResult analyzeMaiCvVideo(const std::string& path,
                                           const MaiCvVideoAnalysisOptions& options,
                                           const MaiToolContext& context);
