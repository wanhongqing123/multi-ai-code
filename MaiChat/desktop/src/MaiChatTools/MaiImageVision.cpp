#include "MaiImageVision.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

namespace {

struct VisionHandle {
  cv::Ptr<cv::FaceDetectorYN> faceDetector;
  cv::dnn::Net personSegmenter;
  std::mutex mutex;
};

char *copyError(const std::string &message) {
  char *result = static_cast<char *>(std::malloc(message.size() + 1));
  if (result)
    std::memcpy(result, message.c_str(), message.size() + 1);
  return result;
}

bool validImage(const unsigned char *pixels, int width, int height,
                int stride) {
  return pixels && width > 0 && height > 0 && width <= 16384 &&
         height <= 16384 && stride >= static_cast<int64_t>(width) * 4 &&
         stride <= std::numeric_limits<int>::max() / height;
}

cv::Mat toBgr(const unsigned char *pixels, int width, int height, int stride) {
  cv::Mat rgba(height, width, CV_8UC4, const_cast<unsigned char *>(pixels),
               stride);
  cv::Mat bgr;
  cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
  return bgr;
}

} // namespace

extern "C" void *maiImageVisionCreate(const unsigned char *faceModel,
                                      size_t faceModelSize,
                                      const unsigned char *personModel,
                                      size_t personModelSize, char **error) {
  if (error)
    *error = nullptr;
  if (!error || !faceModel || !personModel || !faceModelSize ||
      !personModelSize) {
    if (error)
      *error = copyError("Both ONNX models are required");
    return nullptr;
  }
  try {
    auto *handle = new VisionHandle;
    try {
      const std::vector<unsigned char> faceBytes(faceModel,
                                                 faceModel + faceModelSize);
      handle->faceDetector = cv::FaceDetectorYN::create(
          "onnx", faceBytes, {}, cv::Size(320, 320), 0.6f, 0.3f, 5000);
      handle->personSegmenter = cv::dnn::readNetFromONNX(
          reinterpret_cast<const char *>(personModel), personModelSize);
      if (handle->faceDetector.empty() || handle->personSegmenter.empty()) {
        delete handle;
        *error = copyError("OpenCV could not load the image vision models");
        return nullptr;
      }
    } catch (...) {
      delete handle;
      throw;
    }
    return handle;
  } catch (const std::exception &exception) {
    *error = copyError(exception.what());
  } catch (...) {
    *error = copyError("Unknown OpenCV model loading failure");
  }
  return nullptr;
}

extern "C" void maiImageVisionDestroy(void *handle) {
  delete static_cast<VisionHandle *>(handle);
}

extern "C" MaiVisionFacesResult
maiImageVisionDetectFacesRgba(void *handle, const unsigned char *pixels,
                              int width, int height, int stride) {
  MaiVisionFacesResult result{};
  if (!handle || !validImage(pixels, width, height, stride)) {
    result.error = copyError("Invalid face detection input");
    return result;
  }
  try {
    auto *vision = static_cast<VisionHandle *>(handle);
    std::lock_guard<std::mutex> lock(vision->mutex);
    cv::Mat bgr = toBgr(pixels, width, height, stride);
    const float scale = std::min(1.0f, 960.0f / std::max(width, height));
    if (scale < 1.0f)
      cv::resize(bgr, bgr, {}, scale, scale, cv::INTER_AREA);
    vision->faceDetector->setInputSize(bgr.size());
    cv::Mat faces;
    vision->faceDetector->detect(bgr, faces);
    if (faces.empty())
      return result;
    result.count = faces.rows;
    result.faces = static_cast<MaiVisionFace *>(
        std::calloc(static_cast<size_t>(result.count), sizeof(MaiVisionFace)));
    if (!result.faces) {
      result.count = 0;
      result.error = copyError("Face result allocation failed");
      return result;
    }
    for (int row = 0; row < result.count; ++row) {
      const float *detection = faces.ptr<float>(row);
      MaiVisionFace &face = result.faces[row];
      face.x = detection[0] / scale;
      face.y = detection[1] / scale;
      face.width = detection[2] / scale;
      face.height = detection[3] / scale;
      for (int point = 0; point < 10; ++point)
        face.landmarks[point] = detection[point + 4] / scale;
      face.confidence = detection[14];
    }
  } catch (const std::exception &exception) {
    std::free(result.faces);
    result = {};
    result.error = copyError(exception.what());
  } catch (...) {
    std::free(result.faces);
    result = {};
    result.error = copyError("Unknown face detection failure");
  }
  return result;
}

extern "C" MaiVisionMaskResult
maiImageVisionSegmentPersonRgba(void *handle, const unsigned char *pixels,
                                int width, int height, int stride) {
  MaiVisionMaskResult result{};
  if (!handle || !validImage(pixels, width, height, stride)) {
    result.error = copyError("Invalid person segmentation input");
    return result;
  }
  try {
    auto *vision = static_cast<VisionHandle *>(handle);
    std::lock_guard<std::mutex> lock(vision->mutex);
    cv::Mat bgr = toBgr(pixels, width, height, stride);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(192, 192));
    resized.convertTo(resized, CV_32F, 1.0 / 255.0);
    resized -= cv::Scalar(0.5, 0.5, 0.5);
    resized /= cv::Scalar(0.5, 0.5, 0.5);
    cv::Mat blob = cv::dnn::blobFromImage(resized);
    vision->personSegmenter.setInput(blob, "x");
    cv::Mat output =
        vision->personSegmenter.forward("save_infer_model/scale_0.tmp_1");
    if (output.dims != 4 || output.size[0] != 1 || output.size[1] != 2 ||
        output.size[2] != 192 || output.size[3] != 192)
      throw std::runtime_error("Unexpected PPHumanSeg output shape");

    const float *background = output.ptr<float>(0, 0);
    const float *person = output.ptr<float>(0, 1);
    cv::Mat smallMask(192, 192, CV_8UC1);
    for (int row = 0; row < 192; ++row) {
      unsigned char *maskRow = smallMask.ptr<unsigned char>(row);
      for (int column = 0; column < 192; ++column) {
        const int index = row * 192 + column;
        maskRow[column] = person[index] > background[index] ? 255 : 0;
      }
    }
    cv::Mat fullMask;
    cv::resize(smallMask, fullMask, cv::Size(width, height), 0, 0,
               cv::INTER_LINEAR);
    const size_t size = static_cast<size_t>(width) * height;
    result.mask = static_cast<unsigned char *>(std::malloc(size));
    if (!result.mask)
      throw std::bad_alloc();
    if (fullMask.isContinuous()) {
      std::memcpy(result.mask, fullMask.data, size);
    } else {
      for (int row = 0; row < height; ++row)
        std::memcpy(result.mask + static_cast<size_t>(row) * width,
                    fullMask.ptr(row), static_cast<size_t>(width));
    }
    result.width = width;
    result.height = height;
  } catch (const std::exception &exception) {
    std::free(result.mask);
    result = {};
    result.error = copyError(exception.what());
  } catch (...) {
    std::free(result.mask);
    result = {};
    result.error = copyError("Unknown person segmentation failure");
  }
  return result;
}

extern "C" void maiImageVisionFree(void *pointer) { std::free(pointer); }
