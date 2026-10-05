#include "MaiCvImageAnalysis.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTool.h"

namespace {

constexpr std::uint64_t kMaxInputBytes = 30'000'000;
constexpr std::int64_t kMaxPixels = 24'000'000;

cv::Mat decodeImage(const std::string& path, std::string& error) {
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > kMaxInputBytes) {
        error = "Image is empty or exceeds 30 MB";
        return {};
    }
    std::string bytes;
    bool truncated = false;
    const MaiError readError =
        MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, kMaxInputBytes + 1, &truncated);
    if (readError || truncated || bytes.empty()) {
        error = readError ? readError.message() : "Image could not be read completely";
        return {};
    }
    cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1, bytes.data());
    cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (image.empty()) {
        error = "OpenCV could not decode this image; convert it to JPEG or PNG first";
        return {};
    }
    if (static_cast<std::int64_t>(image.cols) * image.rows > kMaxPixels) {
        error = "Decoded image exceeds 24 million pixels";
        return {};
    }
    return image;
}

std::uint64_t differenceHash(const cv::Mat& gray) {
    cv::Mat small;
    cv::resize(gray, small, cv::Size(9, 8), 0, 0, cv::INTER_AREA);
    std::uint64_t hash = 0;
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 8; ++column) {
            hash <<= 1;
            if (small.at<std::uint8_t>(row, column) > small.at<std::uint8_t>(row, column + 1))
                hash |= 1;
        }
    }
    return hash;
}

int hammingDistance(std::uint64_t first, std::uint64_t second) {
    std::uint64_t difference = first ^ second;
    int bits = 0;
    while (difference != 0) {
        difference &= difference - 1;
        ++bits;
    }
    return bits;
}

void writeGrayPng(const cv::Mat& image, const std::string& path, MaiCvImageAnalysisResult& result) {
    std::vector<std::uint8_t> png;
    if (!cv::imencode(".png", image, png)) {
        result.error = "OpenCV could not encode the grayscale PNG";
        return;
    }
    const std::string bytes(reinterpret_cast<const char*>(png.data()), png.size());
    const MaiError written = MaiFileSystem::writeFile(MaiFilePath::fromUtf8(path), bytes);
    if (written) {
        result.error = written.message();
        return;
    }
    result.outputPath = path;
    result.outputBytes = png.size();
}

void findTemplateMatches(const cv::Mat& source, const cv::Mat& pattern,
                         const MaiCvImageAnalysisOptions& options,
                         MaiCvImageAnalysisResult& result) {
    if (pattern.cols > source.cols || pattern.rows > source.rows) {
        result.error = "Template is larger than the source image";
        return;
    }
    if (source.total() > 12'000'000) {
        result.error = "Template matching exceeds 12 million source pixels; resize first";
        return;
    }
    result.templateWidth = pattern.cols;
    result.templateHeight = pattern.rows;
    cv::Mat response;
    cv::matchTemplate(source, pattern, response, cv::TM_SQDIFF_NORMED);
    for (int index = 0; index < options.maxRegions; ++index) {
        double minimum = 0;
        cv::Point location;
        cv::minMaxLoc(response, &minimum, nullptr, &location, nullptr);
        const double score = std::clamp(1.0 - minimum, 0.0, 1.0);
        if (score < options.matchThreshold) break;
        result.regions.push_back({location.x, location.y, pattern.cols, pattern.rows,
                                  static_cast<double>(pattern.total()) / source.total(), score});
        const cv::Rect suppression(location.x - pattern.cols / 2, location.y - pattern.rows / 2,
                                   pattern.cols * 2, pattern.rows * 2);
        response(suppression & cv::Rect(0, 0, response.cols, response.rows)).setTo(1.0);
    }
    result.totalRegions = result.regions.size();
}

void registerTranslation(const cv::Mat& first, const cv::Mat& second,
                         MaiCvImageAnalysisResult& result) {
    if (first.size() != second.size()) {
        result.error = "Images have different dimensions; align or resize them first";
        return;
    }
    if (first.total() > 4'000'000) {
        result.error = "Translation registration exceeds 4 million pixels; resize first";
        return;
    }
    cv::Mat a;
    cv::Mat b;
    first.convertTo(a, CV_32F);
    second.convertTo(b, CV_32F);
    cv::Mat window;
    cv::createHanningWindow(window, first.size(), CV_32F);
    const cv::Point2d shift = cv::phaseCorrelate(a, b, window, &result.registrationResponse);
    result.translationX = shift.x;
    result.translationY = shift.y;
}

void detectLines(const cv::Mat& edges, const MaiCvImageAnalysisOptions& options,
                 MaiCvImageAnalysisResult& result) {
    const int shorterSide = std::min(edges.cols, edges.rows);
    std::vector<cv::Vec4i> found;
    cv::HoughLinesP(edges, found, 1, CV_PI / 180, options.houghThreshold,
                    shorterSide * options.minimumLineLengthFraction,
                    shorterSide * options.maximumLineGapFraction);
    std::vector<MaiCvImageLine> lines;
    lines.reserve(found.size());
    for (const cv::Vec4i& value : found) {
        const double dx = value[2] - value[0];
        const double dy = value[3] - value[1];
        double angle = std::atan2(dy, dx) * 180.0 / CV_PI;
        if (angle > 90) angle -= 180;
        if (angle < -90) angle += 180;
        lines.push_back({{value[0], value[1]}, {value[2], value[3]}, std::hypot(dx, dy), angle});
    }
    std::sort(lines.begin(), lines.end(),
              [](const auto& left, const auto& right) { return left.length > right.length; });
    result.totalLines = lines.size();
    std::vector<double> nearHorizontal;
    for (const auto& line : lines) {
        if (std::abs(line.angleDegrees) <= 30) nearHorizontal.push_back(line.angleDegrees);
    }
    if (!nearHorizontal.empty()) {
        std::sort(nearHorizontal.begin(), nearHorizontal.end());
        result.dominantSkewDegrees = nearHorizontal[nearHorizontal.size() / 2];
        result.hasDominantSkew = true;
    }
    if (lines.size() > static_cast<std::size_t>(options.maxRegions))
        lines.resize(static_cast<std::size_t>(options.maxRegions));
    result.lines = std::move(lines);
}

void detectDocumentCorners(const cv::Mat& edges, MaiCvImageAnalysisResult& result) {
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    const double pixels = static_cast<double>(edges.total());
    double largest = 0;
    std::vector<cv::Point> best;
    for (const auto& contour : contours) {
        const double area = std::abs(cv::contourArea(contour));
        if (area < pixels * 0.1 || area <= largest) continue;
        std::vector<cv::Point> corners;
        cv::approxPolyDP(contour, corners, cv::arcLength(contour, true) * 0.02, true);
        if (corners.size() == 4 && cv::isContourConvex(corners)) {
            largest = area;
            best = std::move(corners);
        }
    }
    if (best.empty()) return;
    cv::Point2d center;
    for (const cv::Point& point : best) {
        center.x += point.x / 4.0;
        center.y += point.y / 4.0;
    }
    std::sort(best.begin(), best.end(), [&center](const cv::Point& left, const cv::Point& right) {
        return std::atan2(left.y - center.y, left.x - center.x) <
               std::atan2(right.y - center.y, right.x - center.x);
    });
    const auto topLeft = std::min_element(best.begin(), best.end(),
                                          [](const cv::Point& left, const cv::Point& right) {
                                              return left.x + left.y < right.x + right.y;
                                          });
    std::rotate(best.begin(), topLeft, best.end());
    for (const auto& point : best) result.documentCorners.push_back({point.x, point.y});
    result.documentAreaFraction = largest / pixels;
    result.documentFound = true;
}

}  // namespace

MaiCvImageAnalysisResult analyzeMaiCvImage(const std::string& firstPath,
                                           const std::string& secondPath,
                                           const MaiCvImageAnalysisOptions& options,
                                           const MaiToolContext& context) {
    maiAssertBlockingAllowed("cv_image_analysis");
    MaiCvImageAnalysisResult result;
    if (context.isCanceled()) {
        result.error = "canceled";
        return result;
    }
    try {
        cv::Mat first = decodeImage(firstPath, result.error);
        if (!result.error.empty()) return result;
        result.width = first.cols;
        result.height = first.rows;
        cv::Mat gray;
        cv::cvtColor(first, gray, cv::COLOR_BGR2GRAY);
        if (context.isCanceled()) {
            result.error = "canceled";
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::Quality) {
            cv::Mat sample = gray;
            const int longerSide = std::max(gray.cols, gray.rows);
            if (longerSide > 1024) {
                const double scale = 1024.0 / longerSide;
                cv::resize(gray, sample, {}, scale, scale, cv::INTER_AREA);
            }
            cv::Mat laplacian;
            cv::Laplacian(sample, laplacian, CV_64F, 3);
            cv::Scalar mean;
            cv::Scalar deviation;
            cv::meanStdDev(laplacian, mean, deviation);
            result.sharpnessVariance = deviation[0] * deviation[0];
            cv::meanStdDev(sample, mean, deviation);
            result.meanLuma = mean[0] / 255.0;
            result.contrastStddev = deviation[0] / 255.0;
            const double pixels = static_cast<double>(sample.total());
            cv::Mat dark;
            cv::Mat bright;
            cv::compare(sample, cv::Scalar(16), dark, cv::CMP_LE);
            cv::compare(sample, cv::Scalar(239), bright, cv::CMP_GE);
            result.darkFraction = cv::countNonZero(dark) / pixels;
            result.brightFraction = cv::countNonZero(bright) / pixels;
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::Compare) {
            cv::Mat second = decodeImage(secondPath, result.error);
            if (!result.error.empty()) return result;
            result.secondWidth = second.cols;
            result.secondHeight = second.rows;
            if (first.size() != second.size()) {
                result.error = "Images have different dimensions; align them before comparison";
                return result;
            }
            cv::Mat difference;
            cv::absdiff(first, second, difference);
            const cv::Scalar average = cv::mean(difference);
            result.meanAbsoluteDifference = (average[0] + average[1] + average[2]) / (3 * 255.0);
            result.identical = cv::countNonZero(difference.reshape(1)) == 0;
            if (!result.identical) result.psnrDb = cv::PSNR(first, second);
            cv::Mat secondGray;
            cv::cvtColor(second, secondGray, cv::COLOR_BGR2GRAY);
            cv::Mat grayDifference;
            cv::absdiff(gray, secondGray, grayDifference);
            cv::Mat changed;
            cv::compare(grayDifference, cv::Scalar(options.changedPixelThreshold), changed,
                        cv::CMP_GT);
            result.changedPixelFraction =
                cv::countNonZero(changed) / static_cast<double>(gray.total());
            result.dHashHamming = hammingDistance(differenceHash(gray), differenceHash(secondGray));
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::TemplateMatch ||
            options.kind == MaiCvImageAnalysisKind::RegisterTranslation) {
            cv::Mat second = decodeImage(secondPath, result.error);
            if (!result.error.empty()) return result;
            result.secondWidth = second.cols;
            result.secondHeight = second.rows;
            cv::Mat secondGray;
            cv::cvtColor(second, secondGray, cv::COLOR_BGR2GRAY);
            if (context.isCanceled()) {
                result.error = "canceled";
                return result;
            }
            if (options.kind == MaiCvImageAnalysisKind::TemplateMatch)
                findTemplateMatches(gray, secondGray, options, result);
            else
                registerTranslation(gray, secondGray, result);
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::ThresholdMask) {
            cv::Mat mask;
            const int binaryType = options.invertMask ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY;
            if (options.thresholdMethod == MaiCvThresholdMethod::Otsu)
                cv::threshold(gray, mask, 0, 255, binaryType | cv::THRESH_OTSU);
            else if (options.thresholdMethod == MaiCvThresholdMethod::Adaptive)
                cv::adaptiveThreshold(gray, mask, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, binaryType,
                                      31, 7);
            else
                cv::threshold(gray, mask, options.fixedThreshold, 255, binaryType);
            if (options.morphologyKernel > 0) {
                const cv::Mat kernel = cv::getStructuringElement(
                    cv::MORPH_ELLIPSE,
                    cv::Size(options.morphologyKernel, options.morphologyKernel));
                cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
            }
            result.foregroundPixels = static_cast<std::uint64_t>(cv::countNonZero(mask));
            if (context.isCanceled()) {
                result.error = "canceled";
                return result;
            }
            writeGrayPng(mask, options.outputPath, result);
            return result;
        }
        cv::Mat edges;
        cv::Canny(gray, edges, options.edgeLowThreshold, options.edgeHighThreshold);
        result.edgePixels = static_cast<std::uint64_t>(cv::countNonZero(edges));
        if (context.isCanceled()) {
            result.error = "canceled";
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::Edges) {
            writeGrayPng(edges, options.outputPath, result);
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::Lines) {
            detectLines(edges, options, result);
            return result;
        }
        if (options.kind == MaiCvImageAnalysisKind::DocumentCorners) {
            detectDocumentCorners(edges, result);
            return result;
        }
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edges, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        const double pixels = static_cast<double>(gray.total());
        std::vector<MaiCvImageRegion> regions;
        for (const auto& contour : contours) {
            const double area = cv::contourArea(contour);
            if (area / pixels < options.minimumAreaFraction) continue;
            const cv::Rect box = cv::boundingRect(contour);
            regions.push_back({box.x, box.y, box.width, box.height, area / pixels});
        }
        std::sort(regions.begin(), regions.end(),
                  [](const MaiCvImageRegion& left, const MaiCvImageRegion& right) {
                      return left.areaFraction > right.areaFraction;
                  });
        result.totalRegions = regions.size();
        if (regions.size() > static_cast<std::size_t>(options.maxRegions))
            regions.resize(static_cast<std::size_t>(options.maxRegions));
        result.regions = std::move(regions);
        return result;
    } catch (const cv::Exception&) {
        result.error = "OpenCV image analysis failed";
        return result;
    } catch (const std::bad_alloc&) {
        result.error = "Image analysis ran out of memory";
        return result;
    }
}
