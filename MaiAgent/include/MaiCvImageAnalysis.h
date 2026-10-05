#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct MaiToolContext;

enum class MaiCvImageAnalysisKind {
    Quality,
    Compare,
    Contours,
    Edges,
    TemplateMatch,
    RegisterTranslation,
    Lines,
    DocumentCorners,
    ThresholdMask,
};

enum class MaiCvThresholdMethod { Otsu, Adaptive, Fixed };

struct MaiCvImageAnalysisOptions {
    MaiCvImageAnalysisKind kind = MaiCvImageAnalysisKind::Quality;
    int edgeLowThreshold = 60;
    int edgeHighThreshold = 160;
    int changedPixelThreshold = 16;
    double minimumAreaFraction = 0.001;
    int maxRegions = 50;
    double matchThreshold = 0.75;
    int houghThreshold = 50;
    double minimumLineLengthFraction = 0.1;
    double maximumLineGapFraction = 0.02;
    MaiCvThresholdMethod thresholdMethod = MaiCvThresholdMethod::Otsu;
    int fixedThreshold = 127;
    bool invertMask = false;
    int morphologyKernel = 0;
    std::string outputPath;
};

struct MaiCvImagePoint {
    int x = 0;
    int y = 0;
};

struct MaiCvImageLine {
    MaiCvImagePoint first;
    MaiCvImagePoint second;
    double length = 0;
    double angleDegrees = 0;
};

struct MaiCvImageRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double areaFraction = 0;
    double score = 0;
};

struct MaiCvImageAnalysisResult {
    int width = 0;
    int height = 0;
    int secondWidth = 0;
    int secondHeight = 0;
    double sharpnessVariance = 0;
    double meanLuma = 0;
    double contrastStddev = 0;
    double darkFraction = 0;
    double brightFraction = 0;
    double meanAbsoluteDifference = 0;
    double changedPixelFraction = 0;
    double psnrDb = 0;
    bool identical = false;
    int dHashHamming = 0;
    double translationX = 0;
    double translationY = 0;
    double registrationResponse = 0;
    double documentAreaFraction = 0;
    bool documentFound = false;
    double dominantSkewDegrees = 0;
    bool hasDominantSkew = false;
    std::uint64_t edgePixels = 0;
    std::uint64_t foregroundPixels = 0;
    std::size_t totalRegions = 0;
    std::size_t totalLines = 0;
    int templateWidth = 0;
    int templateHeight = 0;
    std::vector<MaiCvImageRegion> regions;
    std::vector<MaiCvImageLine> lines;
    std::vector<MaiCvImagePoint> documentCorners;
    std::string outputPath;
    std::uint64_t outputBytes = 0;
    std::string error;
};

// Analyze one or two already-authorized local images with OpenCV. Input files are never modified.
// Edges and ThresholdMask write one new grayscale PNG to options.outputPath, which the caller must
// resolve below the Agent workspace and reserve exclusively. Other kinds are read-only.
// TemplateMatch and RegisterTranslation use secondPath; the former requires the template no larger
// than the source, while the latter requires identical dimensions. Cancellation is checked between
// decode and analysis. JPEG/PNG/WebP decoding depends on the host OpenCV build; unsupported media
// and images above 24 million decoded pixels return error. Scores are measurements, not semantic
// labels, and OpenCV's 8-bit decode is not ICC/HDR color managed.
MaiCvImageAnalysisResult analyzeMaiCvImage(const std::string& firstPath,
                                           const std::string& secondPath,
                                           const MaiCvImageAnalysisOptions& options,
                                           const MaiToolContext& context);
