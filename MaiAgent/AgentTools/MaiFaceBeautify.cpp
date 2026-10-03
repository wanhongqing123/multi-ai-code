#include "MaiFaceBeautify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <onnxruntime_c_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

constexpr int kDetectorSize = 128;
constexpr int kLandmarkerSize = 256;
constexpr int kLandmarkCount = 478;

struct FaceCandidate {
    cv::Rect2f box;
    cv::Point2f eyes[2];
    float score = 0;
};

using Landmarks = std::array<cv::Point2f, kLandmarkCount>;

float sigmoid(float value) {
    return 1.0f / (1.0f + std::exp(-value));
}

bool isUsable(const cv::Point2f& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

class FaceModels {
public:
    ~FaceModels() {
        if (mApi) {
            if (mDetector) mApi->ReleaseSession(mDetector);
            if (mLandmarker) mApi->ReleaseSession(mLandmarker);
            if (mMemory) mApi->ReleaseMemoryInfo(mMemory);
            if (mOptions) mApi->ReleaseSessionOptions(mOptions);
            if (mEnvironment) mApi->ReleaseEnv(mEnvironment);
        }
#if defined(_WIN32)
        if (mLibrary && mOwnLibrary) FreeLibrary(mLibrary);
#else
        if (mLibrary) dlclose(mLibrary);
#endif
    }

    bool open(const MaiFaceBeautifyOptions& options, std::string& error) {
        const OrtApiBase* base = static_cast<const OrtApiBase*>(options.ortApiBase);
        if (!base) {
#if defined(_WIN32)
            mLibrary =
                options.runtimePath.empty()
                    ? GetModuleHandleW(L"onnxruntime.dll")
                    : LoadLibraryW(MaiFilePath::fromUtf8(options.runtimePath).value().c_str());
            mOwnLibrary = !options.runtimePath.empty();
            if (!mLibrary) {
                error = "ONNX Runtime is unavailable";
                return false;
            }
            auto symbol = GetProcAddress(mLibrary, "OrtGetApiBase");
#else
            mLibrary = options.runtimePath.empty()
                           ? dlopen(nullptr, RTLD_NOW)
                           : dlopen(MaiFilePath::fromUtf8(options.runtimePath).value().c_str(),
                                    RTLD_NOW | RTLD_LOCAL);
            if (!mLibrary) {
                error = "ONNX Runtime is unavailable";
                return false;
            }
            auto symbol = dlsym(mLibrary, "OrtGetApiBase");
#endif
            if (!symbol) {
                error = "ONNX Runtime API is unavailable";
                return false;
            }
            using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
            base = reinterpret_cast<GetApiBase>(symbol)();
        }
        mApi = base ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (!mApi) {
            error = "ONNX Runtime API version is incompatible";
            return false;
        }
        if (!check(mApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "MaiFaceBeautify", &mEnvironment),
                   error) ||
            !check(mApi->CreateSessionOptions(&mOptions), error) ||
            !check(mApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mMemory),
                   error))
            return false;
        const MaiFilePath detectorPath = MaiFilePath::fromUtf8(options.detectorModelPath);
        const MaiFilePath landmarkerPath = MaiFilePath::fromUtf8(options.landmarkModelPath);
        return check(mApi->CreateSession(mEnvironment, detectorPath.value().c_str(), mOptions,
                                         &mDetector),
                     error) &&
               check(mApi->CreateSession(mEnvironment, landmarkerPath.value().c_str(), mOptions,
                                         &mLandmarker),
                     error);
    }

    bool run(bool detector, std::vector<float>& input, int edge,
             std::vector<std::vector<float>>& outputs, std::string& error) const {
        const std::array<int64_t, 4> shape = {1, 3, edge, edge};
        OrtValue* inputValue = nullptr;
        if (!check(mApi->CreateTensorWithDataAsOrtValue(
                       mMemory, input.data(), input.size() * sizeof(float), shape.data(),
                       shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputValue),
                   error))
            return false;
        constexpr std::array<const char*, 1> inputNames = {"input"};
        const std::array<const char*, 2> outputNames =
            detector ? std::array<const char*, 2>{"regressors", "scores"}
                     : std::array<const char*, 2>{"landmarks", "score"};
        std::array<OrtValue*, 2> outputValues{};
        const OrtValue* const inputs[] = {inputValue};
        bool success =
            check(mApi->Run(detector ? mDetector : mLandmarker, nullptr, inputNames.data(), inputs,
                            1, outputNames.data(), outputValues.size(), outputValues.data()),
                  error);
        mApi->ReleaseValue(inputValue);
        if (success) {
            outputs.clear();
            for (OrtValue* value : outputValues) {
                OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
                size_t count = 0;
                float* data = nullptr;
                success = check(mApi->GetTensorTypeAndShape(value, &shapeInfo), error) &&
                          check(mApi->GetTensorShapeElementCount(shapeInfo, &count), error) &&
                          check(mApi->GetTensorMutableData(value, reinterpret_cast<void**>(&data)),
                                error);
                if (shapeInfo) mApi->ReleaseTensorTypeAndShapeInfo(shapeInfo);
                if (!success || !data || count > 1'000'000) {
                    success = false;
                    if (error.empty()) error = "invalid face model output";
                    break;
                }
                outputs.emplace_back(data, data + count);
            }
        }
        for (OrtValue* value : outputValues)
            if (value) mApi->ReleaseValue(value);
        return success;
    }

private:
    bool check(OrtStatus* status, std::string& error) const {
        if (!status) return true;
        error = mApi->GetErrorMessage(status);
        mApi->ReleaseStatus(status);
        return false;
    }

#if defined(_WIN32)
    HMODULE mLibrary = nullptr;
    bool mOwnLibrary = false;
#else
    void* mLibrary = nullptr;
#endif
    const OrtApi* mApi = nullptr;
    OrtEnv* mEnvironment = nullptr;
    OrtSessionOptions* mOptions = nullptr;
    OrtMemoryInfo* mMemory = nullptr;
    OrtSession* mDetector = nullptr;
    OrtSession* mLandmarker = nullptr;
};

std::vector<float> toRgbTensor(const cv::Mat& bgr, float offset, float divisor) {
    std::vector<float> tensor(static_cast<std::size_t>(bgr.rows) * bgr.cols * 3);
    const std::size_t area = static_cast<std::size_t>(bgr.rows) * bgr.cols;
    for (int y = 0; y < bgr.rows; ++y) {
        const auto* row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * bgr.cols + x;
            tensor[index] = (row[x][2] - offset) / divisor;
            tensor[area + index] = (row[x][1] - offset) / divisor;
            tensor[2 * area + index] = (row[x][0] - offset) / divisor;
        }
    }
    return tensor;
}

std::vector<FaceCandidate> detectFaces(const cv::Mat& image, FaceModels& models,
                                       std::string& error) {
    const float scale = static_cast<float>(kDetectorSize) / std::max(image.cols, image.rows);
    const float padX = (kDetectorSize - image.cols * scale) * 0.5f;
    const float padY = (kDetectorSize - image.rows * scale) * 0.5f;
    cv::Mat canvas;
    cv::warpAffine(image, canvas, cv::Matx23f(scale, 0, padX, 0, scale, padY),
                   {kDetectorSize, kDetectorSize});
    auto tensor = toRgbTensor(canvas, 127.5f, 127.5f);
    std::vector<std::vector<float>> outputs;
    if (!models.run(true, tensor, kDetectorSize, outputs, error)) return {};
    if (outputs.size() != 2 || outputs[0].size() != 896 * 16 || outputs[1].size() != 896) {
        error = "unexpected detector output shape";
        return {};
    }
    std::vector<FaceCandidate> candidates;
    for (int index = 0; index < 896; ++index) {
        const float score = sigmoid(outputs[1][index]);
        if (score < 0.5f) continue;
        const int cellWidth = index < 512 ? 16 : 8;
        const int cellIndex = index < 512 ? index / 2 : (index - 512) / 6;
        const float anchorX = (cellIndex % cellWidth + 0.5f) / cellWidth * kDetectorSize;
        const float anchorY = (cellIndex / cellWidth + 0.5f) / cellWidth * kDetectorSize;
        const float* regression = outputs[0].data() + index * 16;
        FaceCandidate candidate;
        const float cx = (anchorX + regression[0] - padX) / scale;
        const float cy = (anchorY + regression[1] - padY) / scale;
        const float width = regression[2] / scale;
        const float height = regression[3] / scale;
        candidate.box = {cx - width * 0.5f, cy - height * 0.5f, width, height};
        for (int eye = 0; eye < 2; ++eye)
            candidate.eyes[eye] = {(anchorX + regression[4 + eye * 2] - padX) / scale,
                                   (anchorY + regression[5 + eye * 2] - padY) / scale};
        candidate.score = score;
        if (width > 20 && height > 20 && isUsable(candidate.eyes[0]) && isUsable(candidate.eyes[1]))
            candidates.push_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });
    std::vector<FaceCandidate> selected;
    for (const auto& candidate : candidates) {
        const cv::Rect2f box = candidate.box;
        bool overlaps = false;
        for (const auto& existing : selected) {
            const float intersection = (box & existing.box).area();
            const float united = box.area() + existing.box.area() - intersection;
            if (united > 0 && intersection / united > 0.3f) overlaps = true;
        }
        if (!overlaps) selected.push_back(candidate);
        if (selected.size() >= 5) break;
    }
    return selected;
}

bool locateFace(const cv::Mat& image, FaceModels& models, const FaceCandidate& face,
                Landmarks& points, std::string& error) {
    const cv::Point2f center(face.box.x + face.box.width * 0.5f,
                             face.box.y + face.box.height * 0.5f);
    const float side = 1.5f * std::max(face.box.width, face.box.height);
    if (side < 20 || !std::isfinite(side)) return false;
    const cv::Point2f eyeLine = face.eyes[1] - face.eyes[0];
    const float angle = std::atan2(eyeLine.y, eyeLine.x) * 180.0f / static_cast<float>(CV_PI);
    cv::Mat affine = cv::getRotationMatrix2D(center, angle, kLandmarkerSize / side);
    affine.at<double>(0, 2) += kLandmarkerSize * 0.5 - center.x;
    affine.at<double>(1, 2) += kLandmarkerSize * 0.5 - center.y;
    cv::Mat crop;
    cv::warpAffine(image, crop, affine, {kLandmarkerSize, kLandmarkerSize});
    auto tensor = toRgbTensor(crop, 0, 255);
    std::vector<std::vector<float>> outputs;
    if (!models.run(false, tensor, kLandmarkerSize, outputs, error)) return false;
    if (outputs.size() != 2 || outputs[0].size() != kLandmarkCount * 3 || outputs[1].size() != 1) {
        error = "unexpected landmarker output shape";
        return false;
    }
    if (sigmoid(outputs[1][0]) < 0.5f) return false;
    cv::Mat inverse;
    cv::invertAffineTransform(affine, inverse);
    const double a = inverse.at<double>(0, 0), b = inverse.at<double>(0, 1);
    const double c = inverse.at<double>(0, 2), d = inverse.at<double>(1, 0);
    const double e = inverse.at<double>(1, 1), f = inverse.at<double>(1, 2);
    for (int index = 0; index < kLandmarkCount; ++index) {
        const float x = outputs[0][index * 3];
        const float y = outputs[0][index * 3 + 1];
        points[index] = {static_cast<float>(a * x + b * y + c),
                         static_cast<float>(d * x + e * y + f)};
        if (!isUsable(points[index])) {
            error = "invalid face landmark coordinate";
            return false;
        }
    }
    return true;
}

constexpr std::array<int, 36> kFaceOval = {
    10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377,
    152, 148, 176, 149, 150, 136, 172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109};
constexpr std::array<int, 8> kLeftJaw = {234, 93, 132, 58, 172, 136, 150, 149};
constexpr std::array<int, 8> kRightJaw = {454, 323, 361, 288, 397, 365, 379, 378};

cv::Point2f midpoint(const Landmarks& points, int a, int b) {
    return (points[a] + points[b]) * 0.5f;
}

void warpFace(cv::Mat& image, const Landmarks& points, const MaiFaceBeautifyOptions& options) {
    if (options.slimFace <= 0 && options.enlargeEye <= 0) return;
    std::vector<cv::Point> oval;
    oval.reserve(kFaceOval.size());
    for (int index : kFaceOval) oval.emplace_back(cv::Point(points[index]));
    cv::Rect roi = cv::boundingRect(oval);
    const int padding = std::max(12, static_cast<int>(roi.width * 0.18f));
    roi += cv::Size(padding * 2, padding * 2);
    roi.x -= padding;
    roi.y -= padding;
    roi &= cv::Rect(0, 0, image.cols, image.rows);
    if (roi.empty()) return;
    cv::Mat mapX(roi.size(), CV_32FC1), mapY(roi.size(), CV_32FC1);
    const cv::Point2f faceCenter = midpoint(points, 10, 152);
    const cv::Point2f leftEye = midpoint(points, 33, 133);
    const cv::Point2f rightEye = midpoint(points, 362, 263);
    const float eyeRadius =
        std::max(8.0f, static_cast<float>(cv::norm(rightEye - leftEye)) * 0.23f);
    const float jawRadius = std::max(10.0f, roi.width * 0.17f);
    for (int y = 0; y < roi.height; ++y) {
        float* xRow = mapX.ptr<float>(y);
        float* yRow = mapY.ptr<float>(y);
        for (int x = 0; x < roi.width; ++x) {
            const cv::Point2f target(static_cast<float>(x + roi.x), static_cast<float>(y + roi.y));
            cv::Point2f source = target;
            if (options.enlargeEye > 0) {
                for (const cv::Point2f eye : {leftEye, rightEye}) {
                    const cv::Point2f delta = target - eye;
                    const float ratio = cv::norm(delta) / eyeRadius;
                    if (ratio < 1)
                        source -= delta * (options.enlargeEye * 0.20f * (1 - ratio * ratio) *
                                           (1 - ratio * ratio));
                }
            }
            if (options.slimFace > 0) {
                for (int index : kLeftJaw) {
                    const cv::Point2f delta = target - points[index];
                    const float ratio = cv::norm(delta) / jawRadius;
                    if (ratio < 1)
                        source +=
                            (points[index] - faceCenter) *
                            (options.slimFace * 0.06f * (1 - ratio * ratio) * (1 - ratio * ratio));
                }
                for (int index : kRightJaw) {
                    const cv::Point2f delta = target - points[index];
                    const float ratio = cv::norm(delta) / jawRadius;
                    if (ratio < 1)
                        source +=
                            (points[index] - faceCenter) *
                            (options.slimFace * 0.06f * (1 - ratio * ratio) * (1 - ratio * ratio));
                }
            }
            xRow[x] = source.x;
            yRow[x] = source.y;
        }
    }
    cv::Mat warped;
    cv::remap(image, warped, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_REFLECT101);
    warped.copyTo(image(roi));
}

void treatSkin(cv::Mat& image, const Landmarks& points, const MaiFaceBeautifyOptions& options) {
    if (options.smooth <= 0 && options.whiten <= 0) return;
    std::vector<cv::Point> oval;
    oval.reserve(kFaceOval.size());
    for (int index : kFaceOval) oval.emplace_back(cv::Point(points[index]));
    cv::Rect roi = cv::boundingRect(oval) & cv::Rect(0, 0, image.cols, image.rows);
    if (roi.empty()) return;
    cv::Mat area = image(roi);
    cv::Mat mask(roi.size(), CV_8UC1, cv::Scalar(0));
    for (cv::Point& point : oval) point -= roi.tl();
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{oval}, cv::Scalar(255));
    cv::Mat ycrcb;
    cv::cvtColor(area, ycrcb, cv::COLOR_BGR2YCrCb);
    cv::Mat skin;
    cv::inRange(ycrcb, cv::Scalar(0, 95, 65), cv::Scalar(255, 185, 170), skin);
    cv::bitwise_and(mask, skin, mask);
    const float eyeRadius =
        std::max(8.0f, static_cast<float>(cv::norm(points[263] - points[33])) * 0.17f);
    for (const cv::Point2f eye : {midpoint(points, 33, 133), midpoint(points, 362, 263)})
        cv::circle(mask, eye - cv::Point2f(roi.tl()), static_cast<int>(eyeRadius), cv::Scalar(0),
                   -1);
    const cv::Point2f mouth = midpoint(points, 61, 291) - cv::Point2f(roi.tl());
    cv::ellipse(mask, mouth,
                {static_cast<int>(eyeRadius * 1.4f), static_cast<int>(eyeRadius * 0.8f)}, 0, 0, 360,
                cv::Scalar(0), -1);
    const int blur = std::max(3, static_cast<int>(roi.width * 0.012f) | 1);
    cv::GaussianBlur(mask, mask, {blur, blur}, 0);
    cv::Mat treated = area.clone();
    if (options.smooth > 0) {
        cv::Mat softened;
        cv::bilateralFilter(treated, softened, 9, 35, 10);
        for (int y = 0; y < roi.height; ++y) {
            auto* dst = treated.ptr<cv::Vec3b>(y);
            const auto* src = softened.ptr<cv::Vec3b>(y);
            const auto* weight = mask.ptr<std::uint8_t>(y);
            for (int x = 0; x < roi.width; ++x) {
                const float alpha = options.smooth * weight[x] / 255.0f;
                for (int channel = 0; channel < 3; ++channel)
                    dst[x][channel] = cv::saturate_cast<std::uint8_t>(
                        dst[x][channel] * (1 - alpha) + src[x][channel] * alpha);
            }
        }
    }
    if (options.whiten > 0) {
        cv::Mat lab;
        cv::cvtColor(treated, lab, cv::COLOR_BGR2Lab);
        for (int y = 0; y < roi.height; ++y) {
            auto* row = lab.ptr<cv::Vec3b>(y);
            const auto* weight = mask.ptr<std::uint8_t>(y);
            for (int x = 0; x < roi.width; ++x) {
                const float gain = options.whiten * weight[x] / 255.0f * 0.18f;
                row[x][0] = cv::saturate_cast<std::uint8_t>(row[x][0] + (255 - row[x][0]) * gain);
            }
        }
        cv::cvtColor(lab, treated, cv::COLOR_Lab2BGR);
    }
    treated.copyTo(area);
}

MaiFaceBeautifyResult failure(std::string code, std::string message) {
    MaiFaceBeautifyResult result;
    result.errorCode = std::move(code);
    result.errorMessage = std::move(message);
    return result;
}

}  // namespace

MaiFaceBeautifyResult maiBeautifyFaceImage(const std::string& inputPath,
                                           const std::string& outputPath,
                                           const MaiFaceBeautifyOptions& options,
                                           const MaiToolContext& context) {
    try {
        std::uint64_t inputSize = 0;
        if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(inputPath), inputSize) ||
            inputSize == 0 || inputSize > 50 * 1024 * 1024)
            return failure("invalid_input", "image is unreadable or exceeds 50 MB");
        MaiFaceDecodedImage decoded;
        cv::Mat source;
        if (options.decodeImage && options.decodeImage(inputPath, decoded) && decoded.width > 0 &&
            decoded.height > 0 && decoded.width <= 8192 && decoded.height <= 8192 &&
            decoded.rgba.size() == static_cast<std::size_t>(decoded.width) * decoded.height * 4) {
            cv::Mat rgba(decoded.height, decoded.width, CV_8UC4, decoded.rgba.data());
            cv::cvtColor(rgba, source, cv::COLOR_RGBA2BGRA);
        }
        if (source.empty()) {
            decoded = {};
            std::string bytes;
            const MaiError read = MaiFileSystem::readFile(MaiFilePath::fromUtf8(inputPath), bytes);
            if (read) return failure("invalid_input", read.message());
            cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1, bytes.data());
            source = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
        }
        if (source.empty() || source.cols > 8192 || source.rows > 8192 || source.depth() != CV_8U ||
            static_cast<std::int64_t>(source.cols) * source.rows > 24'000'000)
            return failure("invalid_input", "image dimensions are unsupported");
        cv::Mat image;
        if (source.channels() == 4)
            cv::cvtColor(source, image, cv::COLOR_BGRA2BGR);
        else if (source.channels() == 1)
            cv::cvtColor(source, image, cv::COLOR_GRAY2BGR);
        else if (source.channels() == 3)
            image = source.clone();
        else
            return failure("invalid_input", "image must have one, three, or four channels");
        FaceModels models;
        std::string error;
        if (!models.open(options, error)) return failure("model_unavailable", error);
        const auto candidates = detectFaces(image, models, error);
        if (!error.empty()) return failure("inference_failed", error);
        int count = 0;
        for (const auto& candidate : candidates) {
            if (context.isCanceled())
                return failure("canceled", "face beautification was canceled");
            Landmarks points;
            if (!locateFace(image, models, candidate, points, error)) {
                if (!error.empty()) return failure("inference_failed", error);
                continue;
            }
            warpFace(image, points, options);
            treatSkin(image, points, options);
            ++count;
        }
        if (!count) return failure("face_not_detected", "no usable face landmarks were found");
        if (context.isCanceled()) return failure("canceled", "face beautification was canceled");
        cv::Mat outputImage = image;
        if (source.channels() == 4) {
            cv::cvtColor(image, outputImage, cv::COLOR_BGR2BGRA);
            std::vector<cv::Mat> sourceChannels;
            std::vector<cv::Mat> outputChannels;
            cv::split(source, sourceChannels);
            cv::split(outputImage, outputChannels);
            outputChannels[3] = sourceChannels[3];
            cv::merge(outputChannels, outputImage);
        }
        std::vector<std::uint8_t> png;
        if (options.encodePng) {
            cv::Mat rgba;
            cv::cvtColor(outputImage, rgba,
                         outputImage.channels() == 4 ? cv::COLOR_BGRA2RGBA : cv::COLOR_BGR2RGBA);
            if (!options.encodePng(rgba.data, rgba.cols, rgba.rows, static_cast<int>(rgba.step),
                                   decoded, png))
                return failure("encode_failed", "could not encode color-managed PNG");
        } else if (!cv::imencode(".png", outputImage, png)) {
            return failure("encode_failed", "could not encode output PNG");
        }
        const std::string outputBytes(reinterpret_cast<const char*>(png.data()), png.size());
        const MaiError write =
            MaiFileSystem::writeFile(MaiFilePath::fromUtf8(outputPath), outputBytes);
        if (write) {
            MaiFileSystem::removeFile(MaiFilePath::fromUtf8(outputPath));
            return failure("write_failed", write.message());
        }
        if (context.isCanceled()) {
            MaiFileSystem::removeFile(MaiFilePath::fromUtf8(outputPath));
            return failure("canceled", "face beautification was canceled");
        }
        MaiFaceBeautifyResult result;
        result.outputPath = outputPath;
        result.faceCount = count;
        return result;
    } catch (const cv::Exception& exception) {
        MaiFileSystem::removeFile(MaiFilePath::fromUtf8(outputPath));
        return failure("processing_failed", exception.what());
    } catch (const std::exception& exception) {
        MaiFileSystem::removeFile(MaiFilePath::fromUtf8(outputPath));
        return failure("processing_failed", exception.what());
    }
}
