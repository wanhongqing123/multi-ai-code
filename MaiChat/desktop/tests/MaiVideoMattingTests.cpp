#include <cstdio>
#include <cstdlib>
#include <string>

#include <QImage>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiVideoMatting.h"

extern "C" {
#include <libavformat/avformat.h>
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 1;
  const char *runtime = std::getenv("MAI_ONNXRUNTIME_LIBRARY");
  if (runtime == nullptr || *runtime == '\0') {
    std::printf("Video matting skipped: MAI_ONNXRUNTIME_LIBRARY is unset\n");
    return 0;
  }
  const std::string input = argv[1];
  const MaiFilePath work = MaiFileSystem::temporaryDirectory().append(
      MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_matting_test_")));
  if (MaiFileSystem::createDirectories(work))
    return 2;
  const std::string output =
      work.append(MaiFilePath::fromUtf8("output.mp4")).toUtf8();
  std::uint64_t sourceBytesBefore = 0;
  if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(input), sourceBytesBefore))
    return 3;
  MaiToolContext context;
  context.root = work.toUtf8();
  MaiVideoMattingOptions options;
  options.solidRgb = {20, 120, 40};
  options.modelPath = argv[2];
  options.runtimePath = runtime;
  MaiVideoMattingResult processed =
      maiMatteVideo(input, output, options, context);
  if (!processed.error.empty() || processed.frames < 2 ||
      processed.audioStreamsCopied != 2 || processed.durationSeconds < 1.5) {
    std::fprintf(stderr, "Video matting failed: %s frames=%d duration=%f\n",
                 processed.error.c_str(), processed.frames,
                 processed.durationSeconds);
    return 4;
  }
  std::uint64_t sourceBytesAfter = 0;
  if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(input),
                               sourceBytesAfter) ||
      sourceBytesAfter != sourceBytesBefore)
    return 5;
  AVFormatContext *inspected = nullptr;
  if (avformat_open_input(&inspected, output.c_str(), nullptr, nullptr) < 0)
    return 6;
  if (avformat_find_stream_info(inspected, nullptr) < 0)
    return 7;
  int videoStreams = 0;
  int audioStreams = 0;
  for (unsigned int index = 0; index < inspected->nb_streams; ++index) {
    const AVCodecParameters *codec = inspected->streams[index]->codecpar;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO &&
        codec->codec_id == AV_CODEC_ID_H264)
      ++videoStreams;
    if (codec->codec_type == AVMEDIA_TYPE_AUDIO &&
        codec->codec_id == AV_CODEC_ID_AAC)
      ++audioStreams;
  }
  avformat_close_input(&inspected);
  options.backgroundKind = MaiMattingBackgroundKind::Blur;
  options.blurSigma = 2;
  const std::string blurred =
      work.append(MaiFilePath::fromUtf8("blur.mp4")).toUtf8();
  const MaiVideoMattingResult blurredResult =
      maiMatteVideo(input, blurred, options, context);
  if (!blurredResult.error.empty() || blurredResult.audioStreamsCopied != 2)
    return 9;
  const std::string imagePath =
      work.append(MaiFilePath::fromUtf8("background.png")).toUtf8();
  QImage background(4, 4, QImage::Format_RGB32);
  background.fill(Qt::red);
  if (!background.save(QString::fromUtf8(imagePath.data(),
                                         static_cast<int>(imagePath.size()))))
    return 10;
  options.backgroundKind = MaiMattingBackgroundKind::Image;
  options.backgroundImagePath = imagePath;
  const std::string replaced =
      work.append(MaiFilePath::fromUtf8("image.mp4")).toUtf8();
  const MaiVideoMattingResult imageResult =
      maiMatteVideo(input, replaced, options, context);
  if (!imageResult.error.empty() || imageResult.audioStreamsCopied != 2)
    return 11;
  MaiFileSystem::removeRecursively(work);
  if (videoStreams != 1 || audioStreams != 2) {
    std::fprintf(
        stderr,
        "Expected one H.264 and two original AAC streams, got %d / %d\n",
        videoStreams, audioStreams);
    return 8;
  }
  return 0;
}
