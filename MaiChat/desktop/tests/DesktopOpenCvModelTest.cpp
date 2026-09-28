#include <cstdio>
#include <vector>

#include <QFile>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include "MaiImageVision.h"

static int failures = 0;
#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

std::vector<uchar> readModel(const char* path) {
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    return {reinterpret_cast<const uchar*>(bytes.constData()),
            reinterpret_cast<const uchar*>(bytes.constData() + bytes.size())};
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::puts("face, person, and sample image paths are required");
        return 1;
    }
    CHECK(cv::getVersionString() == "4.14.0");
    const std::vector<uchar> faceModel = readModel(argv[1]);
    const std::vector<uchar> personModel = readModel(argv[2]);
    CHECK(faceModel.size() > 200000);
    CHECK(personModel.size() > 6000000);
    if (faceModel.empty() || personModel.empty()) return 1;

    try {
        auto detector = cv::FaceDetectorYN::create("onnx", faceModel, {}, cv::Size(320, 320));
        CHECK(!detector.empty());
        cv::Mat blackFace(320, 320, CV_8UC3, cv::Scalar::all(0));
        cv::Mat faces;
        CHECK(detector->detect(blackFace, faces) >= 0);

        cv::dnn::Net person = cv::dnn::readNetFromONNX(personModel);
        CHECK(!person.empty());
        cv::Mat blackPerson(192, 192, CV_8UC3, cv::Scalar::all(0));
        person.setInput(cv::dnn::blobFromImage(blackPerson, 1.0 / 255.0,
                                               cv::Size(192, 192), cv::Scalar(), true));
        const cv::Mat mask = person.forward();
        CHECK(!mask.empty());
        CHECK(mask.dims == 4);

        char* creationError = nullptr;
        void* handle = maiImageVisionCreate(faceModel.data(), faceModel.size(),
                                             personModel.data(), personModel.size(),
                                             &creationError);
        if (creationError != nullptr) std::printf("Vision init: %s\n", creationError);
        CHECK(handle != nullptr);
        maiImageVisionFree(creationError);
        if (handle != nullptr) {
            const cv::Mat lena = cv::imread(argv[3], cv::IMREAD_COLOR);
            CHECK(!lena.empty());
            if (!lena.empty()) {
                cv::Mat rgba;
                cv::cvtColor(lena, rgba, cv::COLOR_BGR2RGBA);
                MaiVisionFacesResult found = maiImageVisionDetectFacesRgba(
                    handle, rgba.data, rgba.cols, rgba.rows, static_cast<int>(rgba.step));
                if (found.error != nullptr) std::printf("Face inference: %s\n", found.error);
                CHECK(found.error == nullptr);
                CHECK(found.count >= 1);
                maiImageVisionFree(found.faces);
                maiImageVisionFree(found.error);

                MaiVisionMaskResult segmented = maiImageVisionSegmentPersonRgba(
                    handle, rgba.data, rgba.cols, rgba.rows, static_cast<int>(rgba.step));
                if (segmented.error != nullptr)
                    std::printf("Person inference: %s\n", segmented.error);
                CHECK(segmented.error == nullptr);
                CHECK(segmented.mask != nullptr);
                CHECK(segmented.width == rgba.cols && segmented.height == rgba.rows);
                maiImageVisionFree(segmented.mask);
                maiImageVisionFree(segmented.error);
            }
            maiImageVisionDestroy(handle);
        }
    } catch (const cv::Exception& error) {
        std::printf("OpenCV model failure: %s\n", error.what());
        ++failures;
    }
    if (failures == 0) std::puts("desktop OpenCV models passed");
    return failures == 0 ? 0 : 1;
}
