#include "MaiVideoCircleMark.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/hash.h>
}

#include "MaiFfmpegTools.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiImageDecode.h"
#include "mai_fftools_embed.h"

struct MaiVideoCircleMarkCancel {
    std::atomic<bool> requested{false};
};

namespace {

#include "data/ai_generated_badge.inc"

constexpr uint64_t kMaximumBytes = 200'000'000;
constexpr int64_t kMaximumDurationMs = 60'000;
constexpr int kMinimumWidth = 160;
constexpr int kMinimumHeight = 64;
constexpr int kBadgeX = 12;
constexpr int kBadgeY = 12;
constexpr char kAiGeneratedTag[] = "mai_ai_generated";
constexpr char kAiLabelTag[] = "mai_ai_label";
constexpr char kAiLabelUtf8[] = "AI\xE7\x94\x9F\xE6\x88\x90";

bool isCanceled(const MaiVideoCircleMarkCancel* cancel) {
    return cancel && cancel->requested.load(std::memory_order_relaxed);
}

void setStatus(MaiVideoCircleMarkResult* result, MaiVideoCircleMarkStatus status, const char* detail) {
    result->status = status;
    std::snprintf(result->detail_utf8, sizeof(result->detail_utf8), "%s", detail);
}

bool hasMp4Suffix(const std::string& path) {
    if (path.size() < 4) return false;
    std::string suffix = path.substr(path.size() - 4);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return suffix == ".mp4";
}

struct ProbedMp4 {
    int64_t durationMs = 0;
    int width = 0;
    int height = 0;
    bool hasH264 = false;
    bool hasMetadataMark = false;
};

bool probeMp4(const std::string& path, ProbedMp4& output) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) return false;
    auto closeFormat = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> format(raw, closeFormat);
    if (avformat_find_stream_info(format.get(), nullptr) < 0 || !format->iformat || !format->iformat->name ||
        std::strstr(format->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == nullptr)
        return false;

    const AVStream* video = nullptr;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        if (stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video = stream;
            break;
        }
    }
    if (!video || !video->codecpar) return false;
    output.width = video->codecpar->width;
    output.height = video->codecpar->height;
    output.hasH264 = video->codecpar->codec_id == AV_CODEC_ID_H264;
    int64_t durationUs = format->duration;
    if (durationUs == AV_NOPTS_VALUE || durationUs <= 0) {
        if (video->duration == AV_NOPTS_VALUE || video->duration <= 0) return false;
        durationUs = av_rescale_q(video->duration, video->time_base, AV_TIME_BASE_Q);
    }
    output.durationMs = (durationUs + 999) / 1000;
    const AVDictionaryEntry* generated = av_dict_get(format->metadata, kAiGeneratedTag, nullptr, 0);
    const AVDictionaryEntry* label = av_dict_get(format->metadata, kAiLabelTag, nullptr, 0);
    output.hasMetadataMark =
        generated && std::strcmp(generated->value, "1") == 0 && label && std::strcmp(label->value, kAiLabelUtf8) == 0;
    return true;
}

bool hasVisibleBadge(const std::string& path, const ProbedMp4& video) {
    const MaiImageDecodeResult image = maiImageDecodeFile(path.c_str(), 2048, 2048);
    if (!image.rgba || image.width <= 0 || image.height <= 0 || image.stride < image.width * 4) {
        if (image.rgba) maiImageDecodeFree(image.rgba);
        return false;
    }
    // The badge corner is translucent after H.264 subsampling; verify interior
    // blue pixels and white glyph pixels across areas instead of one edge pixel.
    const auto countRegion = [&](int left, int top, int right, int bottom, bool seekBlue) {
        const int firstX = std::clamp(left * image.width / video.width, 0, image.width);
        const int firstY = std::clamp(top * image.height / video.height, 0, image.height);
        const int lastX = std::clamp((right * image.width + video.width - 1) / video.width, 0, image.width);
        const int lastY = std::clamp((bottom * image.height + video.height - 1) / video.height, 0, image.height);
        int matches = 0;
        int pixels = 0;
        for (int y = firstY; y < lastY; ++y) {
            for (int x = firstX; x < lastX; ++x) {
                const unsigned char* color = image.rgba + static_cast<size_t>(y) * image.stride + x * 4;
                const bool match = seekBlue ? color[2] > 145 && color[2] > color[0] + 45 && color[2] > color[1] + 35
                                            : color[0] > 175 && color[1] > 175 && color[2] > 175 &&
                                                  std::abs(static_cast<int>(color[0]) - color[1]) < 50 &&
                                                  std::abs(static_cast<int>(color[0]) - color[2]) < 50;
                matches += match ? 1 : 0;
                ++pixels;
            }
        }
        return std::pair<int, int>{matches, pixels};
    };
    const auto blue = countRegion(kBadgeX + 6, kBadgeY + 23, kBadgeX + 31, kBadgeY + 33, true);
    const auto white = countRegion(kBadgeX + 44, kBadgeY + 8, kBadgeX + 112, kBadgeY + 32, false);
    const bool visible = blue.second > 0 && white.second > 0 && blue.first >= std::max(4, blue.second / 4) &&
                         white.first >= std::max(6, white.second / 30);
    maiImageDecodeFree(image.rgba);
    return visible;
}

bool hashFile(const MaiFilePath& path, const MaiVideoCircleMarkCancel* cancel, char* hex) {
#if defined(_WIN32)
    std::unique_ptr<FILE, decltype(&std::fclose)> file(_wfopen(path.value().c_str(), L"rb"), &std::fclose);
#else
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.value().c_str(), "rb"), &std::fclose);
#endif
    if (!file) return false;
    AVHashContext* hash = nullptr;
    if (av_hash_alloc(&hash, "sha256") < 0 || !hash) return false;
    av_hash_init(hash);
    std::vector<unsigned char> buffer(64 * 1024);
    bool success = true;
    while (!isCanceled(cancel)) {
        const size_t count = std::fread(buffer.data(), 1, buffer.size(), file.get());
        if (count > 0) av_hash_update(hash, buffer.data(), count);
        if (count < buffer.size()) {
            success = std::feof(file.get()) != 0 && std::ferror(file.get()) == 0;
            break;
        }
    }
    if (isCanceled(cancel)) success = false;
    if (success) av_hash_final_hex(hash, reinterpret_cast<uint8_t*>(hex), 65);
    av_hash_freep(&hash);
    return success;
}

class TemporaryFile {
public:
    explicit TemporaryFile(MaiFilePath path) : mPath(std::move(path)) {}
    ~TemporaryFile() {
        if (mOwned && MaiFileSystem::exists(mPath)) MaiFileSystem::removeFile(mPath);
    }
    const MaiFilePath& path() const {
        return mPath;
    }
    bool reserve() {
        if (MaiFileSystem::createEmptyFile(mPath).hasError()) return false;
        mOwned = true;
        return true;
    }

private:
    MaiFilePath mPath;
    bool mOwned = false;
};

}  // namespace

extern "C" MaiVideoCircleMarkCancel* mai_video_circle_mark_cancel_create(void) {
    return new (std::nothrow) MaiVideoCircleMarkCancel();
}

extern "C" void mai_video_circle_mark_cancel_request(MaiVideoCircleMarkCancel* cancel) {
    if (cancel) cancel->requested.store(true, std::memory_order_relaxed);
}

extern "C" void mai_video_circle_mark_cancel_destroy(MaiVideoCircleMarkCancel* cancel) {
    delete cancel;
}

extern "C" void mai_video_circle_prepare_marked_mp4(const char* source_absolute_path_utf8,
                                                    const char* target_absolute_path_utf8,
                                                    const MaiVideoCircleMarkCancel* cancel,
                                                    MaiVideoCircleMarkResult* result) {
    if (!result) return;
    *result = {};
    if (!source_absolute_path_utf8 || !target_absolute_path_utf8 || !*source_absolute_path_utf8 ||
        !*target_absolute_path_utf8) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_INVALID_ARGUMENT, "Both paths are required.");
        return;
    }
    const std::string sourceUtf8(source_absolute_path_utf8);
    const std::string targetUtf8(target_absolute_path_utf8);
    const MaiFilePath source = MaiFilePath::fromUtf8(sourceUtf8);
    const MaiFilePath target = MaiFilePath::fromUtf8(targetUtf8);
    if (!source.isAbsolute() || !target.isAbsolute() || !hasMp4Suffix(sourceUtf8) || !hasMp4Suffix(targetUtf8) ||
        source == target || MaiFileSystem::resolve(source) == MaiFileSystem::resolve(target) ||
        !MaiFileSystem::isDirectory(target.dirName()) || MaiFileSystem::exists(target)) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_INVALID_ARGUMENT,
                  "Use distinct absolute MP4 paths and a new target file.");
        return;
    }
    if (isCanceled(cancel)) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_CANCELED, "Preparation was canceled.");
        return;
    }
    uint64_t sourceBytes = 0;
    if (!MaiFileSystem::fileSize(source, sourceBytes) || sourceBytes == 0) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_INVALID_SOURCE, "Source file is unavailable.");
        return;
    }
    if (sourceBytes > kMaximumBytes) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_SIZE_LIMIT, "Source exceeds 200 MB.");
        return;
    }
    ProbedMp4 sourceVideo;
    if (!probeMp4(sourceUtf8, sourceVideo) || sourceVideo.width < kMinimumWidth ||
        sourceVideo.height < kMinimumHeight || sourceVideo.width > 8192 || sourceVideo.height > 8192) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_UNSUPPORTED_MP4, "Source must be a playable MP4 with a video track.");
        return;
    }
    if (sourceVideo.durationMs < 1 || sourceVideo.durationMs > kMaximumDurationMs) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_DURATION_LIMIT, "Video duration must be at most 60 seconds.");
        return;
    }

    const std::string nonce = MaiIdGenerator::generate("ai_mark_");
    TemporaryFile badge(MaiFilePath::fromUtf8(targetUtf8 + "." + nonce + ".badge.png"));
    TemporaryFile partial(MaiFilePath::fromUtf8(targetUtf8 + "." + nonce + ".partial.mp4"));
    if (!badge.reserve() || !partial.reserve() ||
        MaiFileSystem::writeFile(badge.path(), std::string(reinterpret_cast<const char*>(kMaiAiGeneratedBadgePng),
                                                           sizeof(kMaiAiGeneratedBadgePng)))
            .hasError()) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_IO_ERROR, "Could not stage the AI badge.");
        return;
    }
    const MaiFfmpegEngine engine{mai_ffmpeg_execute,           mai_ffmpeg_set_cancel_check, mai_ffprobe_execute,
                                 mai_ffprobe_set_cancel_check, mai_fftools_set_log_sink,    mai_fftools_error_string};
    std::vector<std::string> arguments{"ffmpeg",
                                       "-nostdin",
                                       "-y",
                                       "-hide_banner",
                                       "-loglevel",
                                       "error",
                                       "-i",
                                       sourceUtf8,
                                       "-loop",
                                       "1",
                                       "-framerate",
                                       "1",
                                       "-i",
                                       badge.path().toUtf8(),
                                       "-filter_complex",
                                       "[0:v:0][1:v:0]overlay=12:12:shortest=1:format=auto[v]",
                                       "-map",
                                       "[v]",
                                       "-map",
                                       "0:a?",
                                       "-map_metadata",
                                       "-1",
                                       "-map_metadata:s:v",
                                       "-1",
                                       "-map_metadata:s:a",
                                       "-1",
                                       "-map_chapters",
                                       "-1",
                                       "-c:v",
                                       "libx264",
                                       "-preset",
                                       "veryfast",
                                       "-crf",
                                       "22",
                                       "-pix_fmt",
                                       "yuv420p",
                                       "-c:a",
                                       "copy",
                                       "-metadata",
                                       "mai_ai_generated=1",
                                       "-metadata",
                                       std::string("mai_ai_label=") + kAiLabelUtf8,
                                       "-metadata",
                                       "comment=AI-generated video; visibly labeled by MaiChat",
                                       "-movflags",
                                       "+faststart+use_metadata_tags",
                                       "-f",
                                       "mp4",
                                       partial.path().toUtf8()};
    const MaiFfmpegExecutionResult converted =
        maiRunFfmpegSerialized(engine, std::move(arguments), cancel ? &cancel->requested : nullptr);
    if (isCanceled(cancel) || converted.canceled) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_CANCELED, "Preparation was canceled.");
        return;
    }
    if (converted.exitStatus != 0) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_TRANSCODE_ERROR, "Could not add the AI badge to this MP4.");
        return;
    }
    uint64_t outputBytes = 0;
    if (!MaiFileSystem::fileSize(partial.path(), outputBytes) || outputBytes == 0) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_IO_ERROR, "Marked MP4 was not written.");
        return;
    }
    if (outputBytes > kMaximumBytes) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_SIZE_LIMIT, "Marked MP4 exceeds 200 MB.");
        return;
    }
    ProbedMp4 markedVideo;
    if (!probeMp4(partial.path().toUtf8(), markedVideo) || !markedVideo.hasH264 || !markedVideo.hasMetadataMark ||
        markedVideo.durationMs < 1 || markedVideo.durationMs > kMaximumDurationMs) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_VERIFICATION_ERROR,
                  "Marked MP4 metadata or duration could not be verified.");
        return;
    }
    result->metadata_mark_verified = 1;
    if (!hasVisibleBadge(partial.path().toUtf8(), markedVideo)) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_VERIFICATION_ERROR, "The visible AI badge could not be verified.");
        return;
    }
    result->visible_mark_verified = 1;
    if (!hashFile(partial.path(), cancel, result->sha256_hex)) {
        setStatus(result, isCanceled(cancel) ? MAI_VIDEO_CIRCLE_MARK_CANCELED : MAI_VIDEO_CIRCLE_MARK_IO_ERROR,
                  isCanceled(cancel) ? "Preparation was canceled." : "Could not hash marked MP4.");
        return;
    }
    if (isCanceled(cancel)) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_CANCELED, "Preparation was canceled.");
        return;
    }
    if (MaiFileSystem::publishNewFile(partial.path(), target).hasError()) {
        setStatus(result, MAI_VIDEO_CIRCLE_MARK_IO_ERROR, "Could not commit the marked MP4 at the target path.");
        return;
    }
    result->size_bytes = outputBytes;
    result->duration_ms = markedVideo.durationMs;
    setStatus(result, MAI_VIDEO_CIRCLE_MARK_OK, "Marked MP4 is ready.");
}
