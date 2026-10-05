#include "MaiCvImageAnalysis.h"
#include "MaiCvImageTools.h"

#include <json.hpp>

#include <cstdio>
#include <cmath>
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

std::string imageBytes(int rectangleOffset) {
    constexpr int size = 128;
    std::string image = "P6\n128 128\n255\n";
    image.reserve(image.size() + size * size * 3);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool dark =
                x >= 20 + rectangleOffset && x < 100 + rectangleOffset && y >= 30 && y < 90;
            image.append(3, dark ? '\0' : static_cast<char>(255));
        }
    }
    return image;
}

std::string templateBytes() {
    std::string image = "P6\n32 32\n255\n";
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const bool dark = x + 10 >= 20 && y + 20 >= 30;
            image.append(3, dark ? '\0' : static_cast<char>(255));
        }
    }
    return image;
}

}  // namespace

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_cv_image_real_")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath first = root.append(MaiFilePath::fromUtf8("first.ppm"));
    const MaiFilePath second = root.append(MaiFilePath::fromUtf8("second.ppm"));
    const MaiFilePath pattern = root.append(MaiFilePath::fromUtf8("pattern.ppm"));
    CHECK(!MaiFileSystem::writeFile(first, imageBytes(0)));
    CHECK(!MaiFileSystem::writeFile(second, imageBytes(8)));
    CHECK(!MaiFileSystem::writeFile(pattern, templateBytes()));
    MaiToolContext context;
    context.root = root.toUtf8();

    auto quality = makeMaiCvImageQualityTool(analyzeMaiCvImage);
    const auto qualityResult = quality->execute(R"({"path":"first.ppm"})", context);
    CHECK(!qualityResult.hasError());
    if (!qualityResult.hasError()) {
        const auto data = nlohmann::json::parse(qualityResult.output());
        CHECK(data.at("sharpness_laplacian_variance").get<double>() > 0);
        CHECK(data.at("dark_fraction").get<double>() > 0.1);
        CHECK(data.at("bright_fraction").get<double>() > 0.1);
    }

    auto compare = makeMaiCvImageCompareTool(analyzeMaiCvImage);
    const auto same =
        compare->execute(R"({"path":"first.ppm","second_path":"first.ppm"})", context);
    CHECK(!same.hasError());
    if (!same.hasError()) CHECK(nlohmann::json::parse(same.output()).at("identical") == true);
    const auto different =
        compare->execute(R"({"path":"first.ppm","second_path":"second.ppm"})", context);
    CHECK(!different.hasError());
    if (!different.hasError()) {
        const auto data = nlohmann::json::parse(different.output());
        CHECK(data.at("identical") == false);
        CHECK(data.at("changed_pixel_fraction").get<double>() > 0);
    }

    auto contours = makeMaiCvFindContoursTool(analyzeMaiCvImage);
    const auto regions = contours->execute(R"({"path":"first.ppm"})", context);
    CHECK(!regions.hasError());
    if (!regions.hasError()) {
        const auto data = nlohmann::json::parse(regions.output());
        CHECK(data.at("total_regions").get<int>() >= 1);
        CHECK(data.at("regions").at(0).at("x").get<int>() >= 15);
    }

    auto edges = makeMaiCvDetectEdgesTool(analyzeMaiCvImage);
    const auto edgeMap =
        edges->execute(R"({"path":"first.ppm","output_path":"edges.png"})", context);
    CHECK(!edgeMap.hasError());
    std::string png;
    CHECK(!MaiFileSystem::readFile(root.append(MaiFilePath::fromUtf8("edges.png")), png, 8));
    CHECK(png.size() == 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);

    auto match = makeMaiCvTemplateMatchTool(analyzeMaiCvImage);
    const auto found = match->execute(
        R"({"path":"first.ppm","template_path":"pattern.ppm","match_threshold":0.99})", context);
    CHECK(!found.hasError());
    if (!found.hasError()) {
        const auto data = nlohmann::json::parse(found.output());
        CHECK(!data.at("matches").empty());
        if (!data.at("matches").empty()) {
            CHECK(data.at("matches").at(0).at("x").get<int>() == 10);
            CHECK(data.at("matches").at(0).at("y").get<int>() == 20);
        }
    }

    auto registerTool = makeMaiCvRegisterTranslationTool(analyzeMaiCvImage);
    const auto shift =
        registerTool->execute(R"({"path":"first.ppm","second_path":"second.ppm"})", context);
    CHECK(!shift.hasError());
    if (!shift.hasError()) {
        const auto data = nlohmann::json::parse(shift.output());
        CHECK(std::abs(data.at("translation_x").get<double>() - 8) < 1);
        CHECK(std::abs(data.at("translation_y").get<double>()) < 1);
    }

    auto lines = makeMaiCvDetectLinesTool(analyzeMaiCvImage);
    const auto lineResult = lines->execute(R"({"path":"first.ppm"})", context);
    CHECK(!lineResult.hasError());
    if (!lineResult.hasError()) {
        const auto data = nlohmann::json::parse(lineResult.output());
        CHECK(data.at("total_lines").get<int>() >= 2);
        CHECK(!data.at("dominant_skew_degrees").is_null());
    }

    auto document = makeMaiCvDocumentCornersTool(analyzeMaiCvImage);
    const auto documentResult = document->execute(R"({"path":"first.ppm"})", context);
    CHECK(!documentResult.hasError());
    if (!documentResult.hasError()) {
        const auto data = nlohmann::json::parse(documentResult.output());
        CHECK(data.at("found") == true);
        CHECK(data.at("corners").size() == 4);
        if (data.at("corners").size() == 4) {
            CHECK(data.at("corners").at(0).at("x").get<int>() <
                  data.at("corners").at(1).at("x").get<int>());
            CHECK(data.at("corners").at(1).at("y").get<int>() <
                  data.at("corners").at(2).at("y").get<int>());
        }
    }

    auto mask = makeMaiCvThresholdMaskTool(analyzeMaiCvImage);
    const auto maskResult = mask->execute(
        R"({"path":"first.ppm","output_path":"mask.png","method":"otsu","invert":true})", context);
    CHECK(!maskResult.hasError());
    if (!maskResult.hasError()) {
        const auto data = nlohmann::json::parse(maskResult.output());
        CHECK(data.at("foreground_fraction").get<double>() > 0.2);
        CHECK(data.at("foreground_fraction").get<double>() < 0.4);
    }
    MaiFileSystem::removeRecursively(root);
    return failures == 0 ? 0 : 1;
}
