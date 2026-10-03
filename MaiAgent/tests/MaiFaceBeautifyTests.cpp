#include "MaiFaceBeautify.h"
#include "MaiFaceBeautifyTool.h"

#include <cstdlib>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiFaceImageCodecBridge.h"

namespace {

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) return 2;
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-face-beautify-tests"));
    if (MaiFileSystem::createDirectories(root)) return 2;
    MaiToolContext context;
    context.root = root.toUtf8();
    context.allowOutsideWorkingDirectory = true;
    MaiFaceBeautifyOptions options;
    options.detectorModelPath = argv[2];
    options.landmarkModelPath = argv[3];
    options.runtimePath = argv[4];
    options.decodeImage = maiDecodeFaceImage;
    options.encodePng = maiEncodeFacePng;
    const std::string unchanged = root.append(MaiFilePath::fromUtf8("unchanged.png")).toUtf8();
    const std::string refined = root.append(MaiFilePath::fromUtf8("refined.png")).toUtf8();
    const auto zero = maiBeautifyFaceImage(argv[1], unchanged, options, context);
    if (!require(zero.errorCode.empty() && zero.faceCount > 0,
                 ("zero-strength face inference: " + zero.errorMessage).c_str()))
        return 1;
    MaiFaceDecodedImage decodedOriginal;
    if (!require(maiDecodeFaceImage(argv[1], decodedOriginal), "could not decode test portrait"))
        return 1;
    cv::Mat originalRgba(decodedOriginal.height, decodedOriginal.width, CV_8UC4,
                         decodedOriginal.rgba.data());
    cv::Mat original;
    cv::cvtColor(originalRgba, original, cv::COLOR_RGBA2BGR);
    const cv::Mat zeroImage = cv::imread(unchanged, cv::IMREAD_COLOR);
    if (!require(!original.empty() && !zeroImage.empty() &&
                     cv::countNonZero(original.reshape(1) != zeroImage.reshape(1)) == 0,
                 "strength 0 changed decoded image pixels"))
        return 1;
    options.smooth = 0.6f;
    options.whiten = 0.3f;
    options.slimFace = 0.3f;
    options.enlargeEye = 0.3f;
    const auto changed = maiBeautifyFaceImage(argv[1], refined, options, context);
    if (!require(changed.errorCode.empty() && changed.faceCount > 0,
                 ("face beautification: " + changed.errorMessage).c_str()))
        return 1;
    const cv::Mat refinedImage = cv::imread(refined, cv::IMREAD_COLOR);
    if (!require(!refinedImage.empty() &&
                     cv::countNonZero(original.reshape(1) != refinedImage.reshape(1)) > 0,
                 "face effects did not alter the portrait"))
        return 1;
    if (!require(original.at<cv::Vec3b>(0, 0) == refinedImage.at<cv::Vec3b>(0, 0),
                 "background outside the face changed"))
        return 1;
    cv::Mat blank(256, 256, CV_8UC3, cv::Scalar(80, 80, 80));
    const std::string blankPath = root.append(MaiFilePath::fromUtf8("blank.png")).toUtf8();
    const std::string missingPath = root.append(MaiFilePath::fromUtf8("missing.png")).toUtf8();
    cv::imwrite(blankPath, blank);
    const auto missing = maiBeautifyFaceImage(blankPath, missingPath, options, context);
    if (!require(missing.errorCode == "face_not_detected" &&
                     !MaiFileSystem::exists(MaiFilePath::fromUtf8(missingPath)),
                 "no-face image must fail without writing output"))
        return 1;
    auto tool = makeMaiFaceBeautifyTool(maiBeautifyFaceImage, argv[2], argv[3], argv[4], nullptr,
                                        maiDecodeFaceImage, maiEncodeFacePng);
    if (!require(tool && tool->name() == "mobile_beautify_face", "Agent tool is missing")) return 1;
    const nlohmann::json request = {{"image", argv[1]}, {"preset", "natural"}, {"strength", 0}};
    const MaiToolResult toolOutput = tool->execute(request.dump(), context);
    if (!require(!toolOutput.hasError(),
                 ("Agent tool execution: " + toolOutput.error().message()).c_str()))
        return 1;
    const auto result = nlohmann::json::parse(toolOutput.output());
    if (!require(result.value("mime_type", "") == "image/png" &&
                     MaiFileSystem::exists(MaiFilePath::fromUtf8(result.value("path", ""))),
                 "Agent tool did not return a new image"))
        return 1;
    if (argc == 6) {
        const std::string heicOutput =
            root.append(MaiFilePath::fromUtf8("heic-output.png")).toUtf8();
        const auto heic = maiBeautifyFaceImage(argv[5], heicOutput, options, context);
        if (!require(heic.errorCode.empty() || heic.errorCode == "face_not_detected",
                     ("HEIC decode: " + heic.errorCode + " " + heic.errorMessage).c_str()))
            return 1;
    }
    std::cout << "face counts " << zero.faceCount << '/' << changed.faceCount << '\n';
    return 0;
}
