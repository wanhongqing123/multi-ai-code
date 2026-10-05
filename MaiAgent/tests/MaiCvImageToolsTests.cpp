#include "MaiCvImageTools.h"

#include <json.hpp>

#include <cstdio>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                                  \
        }                                                                                \
    } while (false)

MaiCvImageAnalysisResult fakeAnalyzer(const std::string& first, const std::string& second,
                                      const MaiCvImageAnalysisOptions& options,
                                      const MaiToolContext&) {
    MaiCvImageAnalysisResult result;
    result.width = 512;
    result.height = 512;
    if (first.empty()) result.error = "missing first path";
    if (options.kind == MaiCvImageAnalysisKind::Quality) {
        result.sharpnessVariance = 80;
        result.meanLuma = 0.5;
    } else if (options.kind == MaiCvImageAnalysisKind::Compare) {
        if (second.empty()) result.error = "missing second path";
        result.secondWidth = 512;
        result.secondHeight = 512;
        result.changedPixelFraction = 0.25;
        result.psnrDb = 20;
    } else if (options.kind == MaiCvImageAnalysisKind::Contours) {
        result.totalRegions = 3;
        result.regions.push_back({10, 20, 30, 40, 0.02});
    } else {
        result.outputPath = options.outputPath;
        result.outputBytes = 64;
    }
    return result;
}

void testImageTools() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_cv_image_")));
    CHECK(!MaiFileSystem::createDirectories(root));
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("one.png")), "one"));
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("two.png")), "two"));
    MaiToolContext context;
    context.root = root.toUtf8();
    auto quality = makeMaiCvImageQualityTool(fakeAnalyzer);
    auto compare = makeMaiCvImageCompareTool(fakeAnalyzer);
    auto contours = makeMaiCvFindContoursTool(fakeAnalyzer);
    auto edges = makeMaiCvDetectEdgesTool(fakeAnalyzer);
    auto match = makeMaiCvTemplateMatchTool(fakeAnalyzer);
    auto registerTool = makeMaiCvRegisterTranslationTool(fakeAnalyzer);
    auto lines = makeMaiCvDetectLinesTool(fakeAnalyzer);
    auto document = makeMaiCvDocumentCornersTool(fakeAnalyzer);
    auto mask = makeMaiCvThresholdMaskTool(fakeAnalyzer);
    CHECK(quality->name() == "cv_image_quality");
    CHECK(compare->name() == "cv_image_compare");
    CHECK(contours->name() == "cv_find_contours");
    CHECK(edges->name() == "cv_detect_edges");
    CHECK(match->name() == "cv_match_template");
    CHECK(registerTool->name() == "cv_register_translation");
    CHECK(lines->name() == "cv_detect_lines");
    CHECK(document->name() == "cv_document_corners");
    CHECK(mask->name() == "cv_threshold_mask");
    for (MaiTool* tool : {quality.get(), compare.get(), contours.get(), edges.get(), match.get(),
                          registerTool.get(), lines.get(), document.get(), mask.get()}) {
        const auto schema = nlohmann::json::parse(tool->parametersSchema(), nullptr, false);
        CHECK(schema.is_object());
        if (schema.is_object()) CHECK(schema.at("additionalProperties") == false);
    }
    CHECK(nlohmann::json::parse(quality->execute(R"({"path":"one.png"})", context).output())
              .at("sharpness_laplacian_variance") == 80);
    CHECK(quality->execute(R"({"path":"../one.png"})", context).hasError());
    CHECK(compare->execute(R"({"path":"one.png"})", context).hasError());
    CHECK(nlohmann::json::parse(
              compare->execute(R"({"path":"one.png","second_path":"two.png"})", context).output())
              .at("changed_pixel_fraction") == 0.25);
    CHECK(compare->execute(R"({"path":"one.png","second_path":"../two.png"})", context).hasError());
    CHECK(contours->execute(R"({"path":"one.png","edge_low":200,"edge_high":100})", context)
              .hasError());
    const MaiToolResult regions =
        contours->execute(R"({"path":"one.png","max_regions":1})", context);
    CHECK(!regions.hasError());
    CHECK(regions.isTruncated());
    CHECK(nlohmann::json::parse(regions.output()).at("regions").size() == 1);
    CHECK(
        edges->execute(R"({"path":"one.png","output_path":"../outside.png"})", context).hasError());
    CHECK(edges->execute(R"({"path":"one.png","output_path":12})", context).hasError());
    const MaiToolResult edgeMap =
        edges->execute(R"({"path":"one.png","output_path":"edges.png"})", context);
    CHECK(!edgeMap.hasError());
    CHECK(nlohmann::json::parse(edgeMap.output()).at("mime_type") == "image/png");
    CHECK(edges->execute(R"({"path":"one.png","output_path":"edges.png"})", context).hasError());
    CHECK(match->execute(R"({"path":"one.png","template_path":"../two.png"})", context).hasError());
    CHECK(
        match->execute(R"({"path":"one.png","template_path":"two.png","max_matches":51})", context)
            .hasError());
    CHECK(registerTool->execute(R"({"path":"one.png"})", context).hasError());
    CHECK(lines->execute(R"({"path":"one.png","hough_threshold":301})", context).hasError());
    CHECK(mask->execute(R"({"path":"one.png","close_kernel":4})", context).hasError());
    CHECK(mask->execute(R"({"path":"one.png","output_path":"../mask.png"})", context).hasError());
    MaiFileSystem::removeRecursively(root);
}

}  // namespace

int main() {
    testImageTools();
    return failures == 0 ? 0 : 1;
}
