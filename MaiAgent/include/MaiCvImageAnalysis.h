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
    bool alignedIdentical = false;
    bool aspectRatioChanged = false;
    std::string comparisonAlignment = "none";
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

// 用 OpenCV 分析一张或两张已获访问权限的本地图像，不修改任何输入文件。
// Edges、ThresholdMask 会在 options.outputPath 创建新的灰度 PNG；调用方须把目标路径
// 限制在 Agent 工作区内并独占预留。其他分析只读取输入。
// Compare 遇到尺寸不同的图片时，仅在内存中将第二张缩放到第一张的尺寸；结果会区分
// “原像素完全相同”和“缩放后相同”，不会自动校正画面位移。
// TemplateMatch 要求模板不大于源图；RegisterTranslation 要求两张图尺寸一致。
// 解码和分析之间会检查取消。JPEG/PNG/WebP 能否解码取决于宿主 OpenCV 构建；
// 不支持的格式或超过 2400 万像素的图像返回错误。数值指标不是语义判断，
// OpenCV 的 8-bit 解码也不做 ICC/HDR 色彩管理。
MaiCvImageAnalysisResult analyzeMaiCvImage(const std::string& firstPath,
                                           const std::string& secondPath,
                                           const MaiCvImageAnalysisOptions& options,
                                           const MaiToolContext& context);
