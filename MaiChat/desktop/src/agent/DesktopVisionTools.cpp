#include "agent/DesktopVisionTools.h"

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtMath>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiImageVision.h"
#include "MaiTool.h"

namespace {

constexpr std::uint64_t kMaxImageBytes = 50u * 1024 * 1024;
constexpr std::int64_t kMaxPixels = 12'000'000;

std::string utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

MaiToolResult jsonResult(const QJsonObject& object) {
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return MaiToolResult::success(
        std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

class DesktopVisionService final {
public:
    ~DesktopVisionService() { maiImageVisionDestroy(mHandle); }

    void* handle() {
        std::call_once(mInitialize, [this] {
            QFile face(QStringLiteral(":/maichat/models/face_detection_yunet_2023mar.onnx"));
            QFile person(QStringLiteral(":/maichat/models/human_segmentation_pphumanseg_2023mar.onnx"));
            if (!face.open(QIODevice::ReadOnly) || !person.open(QIODevice::ReadOnly)) {
                mError = "bundled image vision models are unavailable";
                return;
            }
            const QByteArray faceBytes = face.readAll();
            const QByteArray personBytes = person.readAll();
            char* error = nullptr;
            mHandle = maiImageVisionCreate(
                reinterpret_cast<const unsigned char*>(faceBytes.constData()),
                static_cast<std::size_t>(faceBytes.size()),
                reinterpret_cast<const unsigned char*>(personBytes.constData()),
                static_cast<std::size_t>(personBytes.size()), &error);
            if (error != nullptr) mError = error;
            maiImageVisionFree(error);
            if (mHandle == nullptr && mError.empty()) mError = "image vision initialization failed";
        });
        return mHandle;
    }

    const std::string& error() const { return mError; }

private:
    std::once_flag mInitialize;
    void* mHandle = nullptr;
    std::string mError;
};

enum class VisionAction { DetectFaces, SegmentPerson };

class DesktopVisionTool final : public MaiTool {
public:
    DesktopVisionTool(VisionAction action, std::shared_ptr<DesktopVisionService> service)
        : mAction(action), mService(std::move(service)) {}

    std::string name() const override {
        return mAction == VisionAction::DetectFaces ? "detect_faces" : "segment_person";
    }

    std::string description() const override {
        return mAction == VisionAction::DetectFaces
            ? "Locate faces and landmarks in an image inside the working directory. Returns "
              "top-left-origin pixel coordinates without changing the image."
            : "Create a grayscale person mask from an image inside the working directory. "
              "The source stays unchanged and a new PNG mask is written to the workspace.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string","description":"Image path inside the working directory"}},"required":["path"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return mAction == VisionAction::SegmentPerson;
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray(argumentsJson.data(), static_cast<int>(argumentsJson.size())), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            document.object().size() != 1 ||
            !document.object().value(QStringLiteral("path")).isString()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "a single image path string is required");
        }
        const QString relative = document.object().value(QStringLiteral("path")).toString();
        const std::string resolved = maiResolvePathWithinRoot(context.root, utf8(relative));
        if (resolved.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image path must stay inside the working directory");
        std::uint64_t fileBytes = 0;
        if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(resolved), fileBytes) ||
            fileBytes == 0 || fileBytes > kMaxImageBytes) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image file is missing or exceeds 50 MB");
        }
        QImageReader reader(QString::fromUtf8(resolved.c_str()));
        reader.setAutoTransform(true);
        const QSize dimensions = reader.size();
        if (dimensions.isValid() &&
            static_cast<std::int64_t>(dimensions.width()) * dimensions.height() > kMaxPixels) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image cannot exceed 12 megapixels");
        }
        const QImage decoded = reader.read();
        if (decoded.isNull())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image format could not be decoded");
        if (static_cast<std::int64_t>(decoded.width()) * decoded.height() > kMaxPixels)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image cannot exceed 12 megapixels");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "image analysis was canceled");
        const QImage rgba = decoded.convertToFormat(QImage::Format_RGBA8888);
        void* handle = mService->handle();
        if (handle == nullptr)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, mService->error());
        return mAction == VisionAction::DetectFaces
            ? detectFaces(handle, rgba, relative)
            : segmentPerson(handle, rgba, context, relative);
    }

private:
    MaiToolResult detectFaces(void* handle, const QImage& rgba,
                              const QString& relative) const {
        MaiVisionFacesResult result = maiImageVisionDetectFacesRgba(
            handle, rgba.constBits(), rgba.width(), rgba.height(), rgba.bytesPerLine());
        if (result.error != nullptr) {
            const std::string error(result.error);
            maiImageVisionFree(result.faces);
            maiImageVisionFree(result.error);
            return MaiToolResult::failure(MaiErrorCode::Internal, error);
        }
        QJsonArray faces;
        if (result.count > 0 && result.faces == nullptr)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "face detector returned no face array");
        const char* labels[] = {"right_eye", "left_eye", "nose", "right_mouth", "left_mouth"};
        for (int index = 0; index < result.count; ++index) {
            const MaiVisionFace& face = result.faces[index];
            QJsonObject landmarks;
            for (int point = 0; point < 5; ++point) {
                landmarks.insert(QString::fromLatin1(labels[point]),
                                 QJsonObject{{QStringLiteral("x"), qRound(face.landmarks[point * 2])},
                                             {QStringLiteral("y"), qRound(face.landmarks[point * 2 + 1])}});
            }
            faces.append(QJsonObject{
                {QStringLiteral("box"),
                 QJsonObject{{QStringLiteral("x"), qRound(face.x)},
                             {QStringLiteral("y"), qRound(face.y)},
                             {QStringLiteral("width"), qRound(face.width)},
                             {QStringLiteral("height"), qRound(face.height)}}},
                {QStringLiteral("confidence"), face.confidence},
                {QStringLiteral("landmarks"), landmarks}});
        }
        maiImageVisionFree(result.faces);
        return jsonResult(QJsonObject{{QStringLiteral("source_width"), rgba.width()},
                                      {QStringLiteral("source_height"), rgba.height()},
                                      {QStringLiteral("source_path"), relative},
                                      {QStringLiteral("coordinate_origin"), QStringLiteral("top_left")},
                                      {QStringLiteral("faces"), faces}});
    }

    MaiToolResult segmentPerson(void* handle, const QImage& rgba,
                                const MaiToolContext& context, const QString& relative) const {
        MaiVisionMaskResult result = maiImageVisionSegmentPersonRgba(
            handle, rgba.constBits(), rgba.width(), rgba.height(), rgba.bytesPerLine());
        if (result.error != nullptr || result.mask == nullptr) {
            const std::string error = result.error != nullptr
                ? std::string(result.error) : "person segmentation returned no mask";
            maiImageVisionFree(result.mask);
            maiImageVisionFree(result.error);
            return MaiToolResult::failure(MaiErrorCode::Internal, error);
        }
        const QImage mask(result.mask, result.width, result.height, result.width,
                          QImage::Format_Grayscale8);
        const QImage owned = mask.copy();
        maiImageVisionFree(result.mask);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "person segmentation was canceled");
        if (owned.isNull())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "person mask could not be copied");
        const std::string name = MaiIdGenerator::generate("person-mask-") + ".png";
        const std::string output = maiResolvePathWithinRoot(context.root, name);
        if (output.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "person mask output is outside the working directory");
        QFile file(QString::fromUtf8(output.c_str()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "person mask output could not be created");
        QImageWriter writer(&file, "PNG");
        if (!writer.write(owned)) {
            file.close();
            file.remove();
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "person mask PNG could not be written");
        }
        return jsonResult(QJsonObject{{QStringLiteral("path"), QString::fromStdString(name)},
                                      {QStringLiteral("mime_type"), QStringLiteral("image/png")},
                                      {QStringLiteral("kind"), QStringLiteral("person_mask")},
                                      {QStringLiteral("source_path"), relative},
                                      {QStringLiteral("source_width"), rgba.width()},
                                      {QStringLiteral("source_height"), rgba.height()},
                                      {QStringLiteral("mask_width"), result.width},
                                      {QStringLiteral("mask_height"), result.height},
                                      {QStringLiteral("next_tool"), QStringLiteral("view_image")}});
    }

    VisionAction mAction;
    std::shared_ptr<DesktopVisionService> mService;
};

}  // namespace

void registerDesktopVisionTools(MaiToolRegistry& registry) {
    auto service = std::make_shared<DesktopVisionService>();
    registry.add(std::make_unique<DesktopVisionTool>(VisionAction::DetectFaces, service));
    registry.add(std::make_unique<DesktopVisionTool>(VisionAction::SegmentPerson, service));
}
