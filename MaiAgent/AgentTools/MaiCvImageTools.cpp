#include "MaiCvImageTools.h"

#include <json.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

class MaiCvImageTool final : public MaiTool {
public:
    MaiCvImageTool(MaiCvImageAnalysisKind kind, MaiCvImageAnalyzer analyzer)
        : mKind(kind), mAnalyzer(analyzer) {}

    std::string name() const override {
        switch (mKind) {
            case MaiCvImageAnalysisKind::Quality: return "cv_image_quality";
            case MaiCvImageAnalysisKind::Compare: return "cv_image_compare";
            case MaiCvImageAnalysisKind::Contours: return "cv_find_contours";
            case MaiCvImageAnalysisKind::Edges: return "cv_detect_edges";
            case MaiCvImageAnalysisKind::TemplateMatch: return "cv_match_template";
            case MaiCvImageAnalysisKind::RegisterTranslation: return "cv_register_translation";
            case MaiCvImageAnalysisKind::Lines: return "cv_detect_lines";
            case MaiCvImageAnalysisKind::DocumentCorners: return "cv_document_corners";
            case MaiCvImageAnalysisKind::ThresholdMask: return "cv_threshold_mask";
        }
        return {};
    }

    std::string description() const override {
        switch (mKind) {
            case MaiCvImageAnalysisKind::Quality:
                return "Measure image sharpness, brightness, contrast, shadow clipping and "
                       "highlight clipping using OpenCV. Scores are numerical measurements, "
                       "not an absolute good/bad judgment. Input is unchanged.";
            case MaiCvImageAnalysisKind::Compare:
                return "Compare two same-size images with mean pixel difference, changed-pixel "
                       "fraction, PSNR and a perceptual dHash distance. Does not align images "
                       "or perform ICC/HDR color management. Inputs are unchanged.";
            case MaiCvImageAnalysisKind::Contours:
                return "Find prominent external contours and bounding rectangles in an image. "
                       "These are geometric regions, not semantic objects or people.";
            case MaiCvImageAnalysisKind::Edges:
                return "Create a new grayscale Canny edge-map PNG in the Agent workspace. "
                       "Useful for checking outlines and boundaries; source is unchanged.";
            case MaiCvImageAnalysisKind::TemplateMatch:
                return "Locate an exact-scale template image within a larger image. Returns "
                       "pixel bounding boxes and similarity scores. It is not semantic search "
                       "and does not handle large scale or perspective changes.";
            case MaiCvImageAnalysisKind::RegisterTranslation:
                return "Estimate horizontal and vertical translation between two same-size "
                       "images using phase correlation. Returns shift and confidence without "
                       "warping either image.";
            case MaiCvImageAnalysisKind::Lines:
                return "Detect straight line segments with OpenCV Hough transform and estimate "
                       "a near-horizontal skew angle when possible. Returns pixel coordinates; "
                       "does not rotate the source.";
            case MaiCvImageAnalysisKind::DocumentCorners:
                return "Find the four corners of the largest clear document-like quadrilateral. "
                       "Returns ordered pixel points for later perspective correction; a photo "
                       "without a clear border may return found=false.";
            case MaiCvImageAnalysisKind::ThresholdMask:
                return "Create a new grayscale binary PNG mask using Otsu, adaptive, or fixed "
                       "thresholding, optional inversion and small-gap closing. Original image "
                       "and its color profile are unchanged.";
        }
        return {};
    }

    std::string parametersSchema() const override {
        if (mKind == MaiCvImageAnalysisKind::Compare)
            return R"({"type":"object","properties":{"path":{"type":"string"},"second_path":{"type":"string"},"changed_pixel_threshold":{"type":"integer","minimum":1,"maximum":255}},"required":["path","second_path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::Quality)
            return R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::Contours)
            return R"({"type":"object","properties":{"path":{"type":"string"},"edge_low":{"type":"integer","minimum":1,"maximum":254},"edge_high":{"type":"integer","minimum":2,"maximum":255},"minimum_area_fraction":{"type":"number","minimum":0,"maximum":1},"max_regions":{"type":"integer","minimum":1,"maximum":200}},"required":["path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::TemplateMatch)
            return R"({"type":"object","properties":{"path":{"type":"string"},"template_path":{"type":"string"},"match_threshold":{"type":"number","minimum":0,"maximum":1},"max_matches":{"type":"integer","minimum":1,"maximum":50}},"required":["path","template_path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::RegisterTranslation)
            return R"({"type":"object","properties":{"path":{"type":"string"},"second_path":{"type":"string"}},"required":["path","second_path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::Lines)
            return R"({"type":"object","properties":{"path":{"type":"string"},"edge_low":{"type":"integer","minimum":1,"maximum":254},"edge_high":{"type":"integer","minimum":2,"maximum":255},"hough_threshold":{"type":"integer","minimum":10,"maximum":300},"min_line_fraction":{"type":"number","minimum":0.01,"maximum":1},"max_gap_fraction":{"type":"number","minimum":0,"maximum":1},"max_lines":{"type":"integer","minimum":1,"maximum":200}},"required":["path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::DocumentCorners)
            return R"({"type":"object","properties":{"path":{"type":"string"},"edge_low":{"type":"integer","minimum":1,"maximum":254},"edge_high":{"type":"integer","minimum":2,"maximum":255}},"required":["path"],"additionalProperties":false})";
        if (mKind == MaiCvImageAnalysisKind::ThresholdMask)
            return R"({"type":"object","properties":{"path":{"type":"string"},"output_path":{"type":"string"},"method":{"type":"string","enum":["otsu","adaptive","fixed"]},"fixed_threshold":{"type":"integer","minimum":0,"maximum":255},"invert":{"type":"boolean"},"close_kernel":{"type":"integer","minimum":0,"maximum":31}},"required":["path"],"additionalProperties":false})";
        return R"({"type":"object","properties":{"path":{"type":"string"},"output_path":{"type":"string"},"edge_low":{"type":"integer","minimum":1,"maximum":254},"edge_high":{"type":"integer","minimum":2,"maximum":255}},"required":["path"],"additionalProperties":false})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (mAnalyzer == nullptr)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "OpenCV image analysis is unavailable on this host");
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.contains("path") || !request["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "An accessible local image path is required");
        const std::string first = context.resolvePath(request["path"].get<std::string>());
        if (first.empty() || !MaiFileSystem::exists(MaiFilePath::fromUtf8(first)) ||
            MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(first)))
            return MaiToolResult::failure(
                MaiErrorCode::NotFound, "Image path is unavailable or outside the accessible area");
        std::string second;
        const bool needsSecond = mKind == MaiCvImageAnalysisKind::Compare ||
                                 mKind == MaiCvImageAnalysisKind::TemplateMatch ||
                                 mKind == MaiCvImageAnalysisKind::RegisterTranslation;
        if (needsSecond) {
            const char* secondField =
                mKind == MaiCvImageAnalysisKind::TemplateMatch ? "template_path" : "second_path";
            if (!request.contains(secondField) || !request[secondField].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              std::string(secondField) + " is required");
            second = context.resolvePath(request[secondField].get<std::string>());
            if (second.empty() || !MaiFileSystem::exists(MaiFilePath::fromUtf8(second)) ||
                MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(second)))
                return MaiToolResult::failure(MaiErrorCode::NotFound,
                                              "Second image path is unavailable");
        }
        MaiCvImageAnalysisOptions options;
        options.kind = mKind;
        if (!integer(request, "edge_low", options.edgeLowThreshold, 1, 254) ||
            !integer(request, "edge_high", options.edgeHighThreshold, 2, 255) ||
            !integer(request, "changed_pixel_threshold", options.changedPixelThreshold, 1, 255) ||
            !integer(request, "max_regions", options.maxRegions, 1, 200) ||
            !integer(request, "max_matches", options.maxRegions, 1, 50) ||
            !integer(request, "max_lines", options.maxRegions, 1, 200) ||
            !integer(request, "hough_threshold", options.houghThreshold, 10, 300) ||
            !integer(request, "fixed_threshold", options.fixedThreshold, 0, 255) ||
            !integer(request, "close_kernel", options.morphologyKernel, 0, 31) ||
            !number(request, "minimum_area_fraction", options.minimumAreaFraction, 0, 1) ||
            !number(request, "match_threshold", options.matchThreshold, 0, 1) ||
            !number(request, "min_line_fraction", options.minimumLineLengthFraction, 0.01, 1) ||
            !number(request, "max_gap_fraction", options.maximumLineGapFraction, 0, 1) ||
            options.edgeHighThreshold <= options.edgeLowThreshold ||
            (options.morphologyKernel != 0 &&
             (options.morphologyKernel < 3 || options.morphologyKernel % 2 == 0)))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "Invalid OpenCV analysis parameter range");
        if (mKind == MaiCvImageAnalysisKind::ThresholdMask) {
            if (request.contains("method") && !request["method"].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "method must be otsu, adaptive, or fixed");
            const std::string method = request.value("method", std::string("otsu"));
            if (method == "otsu")
                options.thresholdMethod = MaiCvThresholdMethod::Otsu;
            else if (method == "adaptive")
                options.thresholdMethod = MaiCvThresholdMethod::Adaptive;
            else if (method == "fixed")
                options.thresholdMethod = MaiCvThresholdMethod::Fixed;
            else
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "method must be otsu, adaptive, or fixed");
            if (request.contains("invert") && !request["invert"].is_boolean())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invert must be boolean");
            options.invertMask = request.value("invert", false);
        }
        const bool createsOutput = mKind == MaiCvImageAnalysisKind::Edges ||
                                   mKind == MaiCvImageAnalysisKind::ThresholdMask;
        if (createsOutput) {
            if (request.contains("output_path") && !request["output_path"].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "output_path must be a string");
            std::string relative = request.value("output_path", std::string{});
            if (relative.empty())
                relative = MaiIdGenerator::generate(mKind == MaiCvImageAnalysisKind::ThresholdMask
                                                        ? "cv_mask_"
                                                        : "cv_edges_") +
                           ".png";
            const MaiFilePath relativePath = MaiFilePath::fromUtf8(relative);
            const std::string name = relativePath.baseName().toUtf8();
            if (context.root.empty() || relativePath.isAbsolute() || name.size() < 5 ||
                name.substr(name.size() - 4) != ".png")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "output_path must be a relative .png path");
            options.outputPath = maiResolvePathWithinRoot(context.root, relative);
            if (options.outputPath.empty() ||
                MaiFileSystem::exists(MaiFilePath::fromUtf8(options.outputPath)))
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "output_path is outside the Agent workspace or exists");
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Image analysis was canceled");
        if (createsOutput) {
            const MaiError reserved =
                MaiFileSystem::createEmptyFile(MaiFilePath::fromUtf8(options.outputPath));
            if (reserved) return MaiToolResult::failure(reserved.code(), reserved.message());
        }
        const MaiCvImageAnalysisResult result = mAnalyzer(first, second, options, context);
        if (context.isCanceled() || result.error == "canceled") {
            if (createsOutput)
                (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(options.outputPath));
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Image analysis was canceled");
        }
        if (!result.error.empty()) {
            if (createsOutput)
                (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(options.outputPath));
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, result.error);
        }
        Json output = {{"width", result.width}, {"height", result.height}, {"method", "opencv"}};
        switch (mKind) {
            case MaiCvImageAnalysisKind::Quality:
                output.update(Json{{"sharpness_laplacian_variance", result.sharpnessVariance},
                                   {"mean_luma", result.meanLuma},
                                   {"contrast_stddev", result.contrastStddev},
                                   {"dark_fraction", result.darkFraction},
                                   {"bright_fraction", result.brightFraction},
                                   {"analysis_max_side", 1024},
                                   {"score_note", "Compare scores only between similar images."}});
                break;
            case MaiCvImageAnalysisKind::Compare:
                output.update(
                    Json{{"second_width", result.secondWidth},
                         {"second_height", result.secondHeight},
                         {"identical", result.identical},
                         {"mean_absolute_difference", result.meanAbsoluteDifference},
                         {"changed_pixel_fraction", result.changedPixelFraction},
                         {"dhash_hamming", result.dHashHamming},
                         {"psnr_db", result.identical ? Json(nullptr) : Json(result.psnrDb)},
                         {"color_managed", false}});
                break;
            case MaiCvImageAnalysisKind::Contours: {
                Json regions = Json::array();
                for (const auto& region : result.regions)
                    regions.push_back(Json{{"x", region.x},
                                           {"y", region.y},
                                           {"width", region.width},
                                           {"height", region.height},
                                           {"area_fraction", region.areaFraction}});
                output.update(Json{{"regions", std::move(regions)},
                                   {"total_regions", result.totalRegions},
                                   {"edge_pixels", result.edgePixels},
                                   {"truncated", result.regions.size() < result.totalRegions}});
                break;
            }
            case MaiCvImageAnalysisKind::Edges:
                output.update(Json{{"path", result.outputPath},
                                   {"bytes", result.outputBytes},
                                   {"mime_type", "image/png"},
                                   {"edge_pixels", result.edgePixels}});
                break;
            case MaiCvImageAnalysisKind::TemplateMatch: {
                Json matches = Json::array();
                for (const auto& region : result.regions)
                    matches.push_back(Json{{"x", region.x},
                                           {"y", region.y},
                                           {"width", region.width},
                                           {"height", region.height},
                                           {"score", region.score}});
                output.update(Json{{"matches", std::move(matches)},
                                   {"template_width", result.templateWidth},
                                   {"template_height", result.templateHeight},
                                   {"score_method", "1_minus_sqdiff_normed"}});
                break;
            }
            case MaiCvImageAnalysisKind::RegisterTranslation:
                output.update(Json{{"second_width", result.secondWidth},
                                   {"second_height", result.secondHeight},
                                   {"translation_x", result.translationX},
                                   {"translation_y", result.translationY},
                                   {"response", result.registrationResponse},
                                   {"method", "phase_correlation"}});
                break;
            case MaiCvImageAnalysisKind::Lines: {
                Json lines = Json::array();
                for (const auto& line : result.lines)
                    lines.push_back(Json{{"x1", line.first.x},
                                         {"y1", line.first.y},
                                         {"x2", line.second.x},
                                         {"y2", line.second.y},
                                         {"length", line.length},
                                         {"angle_degrees", line.angleDegrees}});
                output.update(Json{
                    {"lines", std::move(lines)},
                    {"total_lines", result.totalLines},
                    {"dominant_skew_degrees",
                     result.hasDominantSkew ? Json(result.dominantSkewDegrees) : Json(nullptr)},
                    {"truncated", result.lines.size() < result.totalLines}});
                break;
            }
            case MaiCvImageAnalysisKind::DocumentCorners: {
                Json corners = Json::array();
                for (const auto& point : result.documentCorners)
                    corners.push_back(Json{{"x", point.x}, {"y", point.y}});
                output.update(Json{{"found", result.documentFound},
                                   {"corners", std::move(corners)},
                                   {"area_fraction", result.documentAreaFraction},
                                   {"point_order", "top_left,top_right,bottom_right,bottom_left"}});
                break;
            }
            case MaiCvImageAnalysisKind::ThresholdMask:
                output.update(Json{
                    {"path", result.outputPath},
                    {"bytes", result.outputBytes},
                    {"mime_type", "image/png"},
                    {"foreground_pixels", result.foregroundPixels},
                    {"foreground_fraction",
                     result.foregroundPixels / static_cast<double>(result.width * result.height)}});
                break;
        }
        return MaiToolResult::success(output.dump(),
                                      (mKind == MaiCvImageAnalysisKind::Contours &&
                                       result.regions.size() < result.totalRegions) ||
                                          (mKind == MaiCvImageAnalysisKind::Lines &&
                                           result.lines.size() < result.totalLines));
    }

private:
    static bool integer(const Json& request, const char* field, int& output, int minimum,
                        int maximum) {
        if (!request.contains(field)) return true;
        if (!request[field].is_number_integer()) return false;
        const std::int64_t value = request[field].get<std::int64_t>();
        if (value < minimum || value > maximum) return false;
        output = static_cast<int>(value);
        return true;
    }

    static bool number(const Json& request, const char* field, double& output, double minimum,
                       double maximum) {
        if (!request.contains(field)) return true;
        if (!request[field].is_number()) return false;
        output = request[field].get<double>();
        return std::isfinite(output) && output >= minimum && output <= maximum;
    }

    MaiCvImageAnalysisKind mKind;
    MaiCvImageAnalyzer mAnalyzer;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCvImageQualityTool(MaiCvImageAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::Quality, analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvImageCompareTool(MaiCvImageAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::Compare, analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvFindContoursTool(MaiCvImageAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::Contours, analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvDetectEdgesTool(MaiCvImageAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::Edges, analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvTemplateMatchTool(MaiCvImageAnalyzer analyzer) {
    return analyzer
               ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::TemplateMatch, analyzer)
               : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvRegisterTranslationTool(MaiCvImageAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::RegisterTranslation,
                                                       analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvDetectLinesTool(MaiCvImageAnalyzer analyzer) {
    return analyzer ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::Lines, analyzer)
                    : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvDocumentCornersTool(MaiCvImageAnalyzer analyzer) {
    return analyzer
               ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::DocumentCorners, analyzer)
               : nullptr;
}

std::unique_ptr<MaiTool> makeMaiCvThresholdMaskTool(MaiCvImageAnalyzer analyzer) {
    return analyzer
               ? std::make_unique<MaiCvImageTool>(MaiCvImageAnalysisKind::ThresholdMask, analyzer)
               : nullptr;
}
