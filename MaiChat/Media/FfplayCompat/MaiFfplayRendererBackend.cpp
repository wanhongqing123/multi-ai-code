// 文件作用：实现 ffplay 视频帧提交到宿主渲染后端的桥接入口。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "MaiFfplayEntry.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

extern "C" {
#include "ffplay_renderer.h"
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/ambient_viewing_environment.h>
#include <libavutil/mathematics.h>
}

// ffplay 的替代渲染器在 SDL 纹理转换前收到 AVFrame。
// 宿主实现把该帧交给 Graphics，不创建 Vulkan 设备。
struct VkRenderer {
    MaiFfplayHost host{};
    AVBufferRef* hardwareDevice = nullptr;
};

namespace {

bool encodeAmbient(const AVAmbientViewingEnvironment* ambient, uint8_t (&bytes)[8]) {
    if (!ambient || ambient->ambient_illuminance.num < 0 ||
        ambient->ambient_illuminance.den <= 0 || ambient->ambient_light_x.num < 0 ||
        ambient->ambient_light_x.den <= 0 || ambient->ambient_light_y.num < 0 ||
        ambient->ambient_light_y.den <= 0)
        return false;
    const int64_t illuminance = av_rescale_rnd(ambient->ambient_illuminance.num, 10000,
                                               ambient->ambient_illuminance.den,
                                               AV_ROUND_NEAR_INF);
    const int64_t x = av_rescale_rnd(ambient->ambient_light_x.num, 50000,
                                    ambient->ambient_light_x.den, AV_ROUND_NEAR_INF);
    const int64_t y = av_rescale_rnd(ambient->ambient_light_y.num, 50000,
                                    ambient->ambient_light_y.den, AV_ROUND_NEAR_INF);
    if (illuminance < 0 || illuminance > UINT32_MAX || x < 0 || x > UINT16_MAX ||
        y < 0 || y > UINT16_MAX)
        return false;
    const uint32_t lux = static_cast<uint32_t>(illuminance);
    bytes[0] = static_cast<uint8_t>(lux >> 24);
    bytes[1] = static_cast<uint8_t>(lux >> 16);
    bytes[2] = static_cast<uint8_t>(lux >> 8);
    bytes[3] = static_cast<uint8_t>(lux);
    bytes[4] = static_cast<uint8_t>(x >> 8);
    bytes[5] = static_cast<uint8_t>(x);
    bytes[6] = static_cast<uint8_t>(y >> 8);
    bytes[7] = static_cast<uint8_t>(y);
    return true;
}

}  // namespace

extern "C" MAI_FFPLAY_EXPORT int maiFfplayProbeHdrSource(
    const char* path, MaiFfplayHdrSourceInfo* info) {
    if (!info) return 0;
    *info = {};
    if (!path || !*path) return 0;
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path, nullptr, nullptr) < 0) return 0;
    const bool hasStreams = avformat_find_stream_info(input, nullptr) >= 0;
    bool hdr = false;
    bool subtitles = false;
    if (hasStreams) {
        for (unsigned int index = 0; index < input->nb_streams; ++index) {
            const AVCodecParameters* codec = input->streams[index]->codecpar;
            if (codec->codec_type == AVMEDIA_TYPE_SUBTITLE) subtitles = true;
            if (codec->codec_type == AVMEDIA_TYPE_VIDEO &&
                codec->codec_id == AV_CODEC_ID_HEVC &&
                codec->profile == AV_PROFILE_HEVC_MAIN_10 &&
                (codec->sample_aspect_ratio.num == 0 ||
                 codec->sample_aspect_ratio.num == codec->sample_aspect_ratio.den) &&
                (codec->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
                 codec->color_trc == AVCOL_TRC_SMPTE2084)) {
                hdr = true;
                if (codec->color_trc == AVCOL_TRC_ARIB_STD_B67) {
                    info->hlg = 1;
                    const AVPacketSideData* side = av_packet_side_data_get(
                        codec->coded_side_data, codec->nb_coded_side_data,
                        AV_PKT_DATA_AMBIENT_VIEWING_ENVIRONMENT);
                    if (side && side->size >= sizeof(AVAmbientViewingEnvironment)) {
                        const auto* ambient =
                            reinterpret_cast<const AVAmbientViewingEnvironment*>(side->data);
                        info->has_ambient = encodeAmbient(ambient, info->ambient);
                    }
                }
            }
        }
    }
    avformat_close_input(&input);
    info->native_frame_eligible = hdr && !subtitles ? 1 : 0;
    if (!info->native_frame_eligible) info->hlg = info->has_ambient = 0;
    return 1;
}

extern "C" MAI_FFPLAY_EXPORT int maiFfplaySourceHasHdrWithoutSubtitles(const char* path) {
    MaiFfplayHdrSourceInfo info{};
    return maiFfplayProbeHdrSource(path, &info) ? info.native_frame_eligible : 0;
}

extern "C" VkRenderer* vk_get_renderer(void) {
    const MaiFfplayHost host = maiFfplayCurrentHost();
    if (!host.present_native_frames || !host.present_video) return nullptr;
    static VkRenderer renderer;
    av_buffer_unref(&renderer.hardwareDevice);
    renderer.host = host;
    return &renderer;
}

extern "C" int vk_renderer_create(VkRenderer* renderer, SDL_Window*, AVDictionary*) {
    return renderer && renderer->host.present_video ? 0 : AVERROR(EINVAL);
}

extern "C" int vk_renderer_get_hw_dev(VkRenderer* renderer, AVBufferRef** device) {
#if defined(__APPLE__)
    if (!renderer || !device) return AVERROR(EINVAL);
    if (!renderer->hardwareDevice) {
        const int result = av_hwdevice_ctx_create(&renderer->hardwareDevice,
                                                  AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                                  nullptr, nullptr, 0);
        if (result < 0) return result;
    }
    *device = renderer->hardwareDevice;
    return 0;
#else
    return AVERROR(ENOSYS);
#endif
}

// ffplay 使用旧渲染入口提交帧；这里把帧交给宿主图形层，硬件上下文不可用时明确报错。
// 返回值继续遵守 FFmpeg 错误约定，使播放循环能够区分提交失败和正常退出。
extern "C" int vk_renderer_display(VkRenderer* renderer, AVFrame* frame, RenderParams*) {
    if (!renderer || !frame) return AVERROR(EINVAL);
    AVFrame* transferred = nullptr;
#if defined(__APPLE__)
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        transferred = av_frame_alloc();
        if (!transferred) return AVERROR(ENOMEM);
        const int result = av_hwframe_transfer_data(transferred, frame, 0);
        if (result < 0) {
            av_frame_free(&transferred);
            return result;
        }
        av_frame_copy_props(transferred, frame);
        frame = transferred;
    }
#endif
    MaiVideoFrame video{};
    video.width = frame->width;
    video.height = frame->height;
    video.color_space = frame->colorspace == AVCOL_SPC_BT2020_NCL ||
                                frame->colorspace == AVCOL_SPC_BT2020_CL
                            ? MAI_VIDEO_COLOR_BT2020
                            : frame->colorspace == AVCOL_SPC_BT470BG ||
                                      frame->colorspace == AVCOL_SPC_SMPTE170M
                                  ? MAI_VIDEO_COLOR_BT601
                                  : MAI_VIDEO_COLOR_BT709;
    video.color_range = frame->color_range == AVCOL_RANGE_JPEG
                            ? MAI_VIDEO_RANGE_FULL : MAI_VIDEO_RANGE_LIMITED;
    video.color_transfer = frame->color_trc == AVCOL_TRC_ARIB_STD_B67
                               ? MAI_VIDEO_TRANSFER_HLG
                           : frame->color_trc == AVCOL_TRC_SMPTE2084
                               ? MAI_VIDEO_TRANSFER_PQ
                               : MAI_VIDEO_TRANSFER_SDR;
    switch (frame->format) {
        case AV_PIX_FMT_YUV420P10LE: video.format = MAI_VIDEO_PIXEL_I010; break;
        case AV_PIX_FMT_P010LE: video.format = MAI_VIDEO_PIXEL_P010; break;
        case AV_PIX_FMT_YUV420P: video.format = MAI_VIDEO_PIXEL_I420; break;
        case AV_PIX_FMT_NV12: video.format = MAI_VIDEO_PIXEL_NV12; break;
        default:
            av_frame_free(&transferred);
            return AVERROR(ENOSYS);
    }
    for (int index = 0; index < 3; ++index) {
        video.data[index] = frame->data[index];
        video.linesize[index] = frame->linesize[index];
    }
    const int result = renderer->host.present_video(renderer->host.user_data, &video, nullptr)
                           ? 0 : AVERROR(EIO);
    av_frame_free(&transferred);
    return result;
}

extern "C" int vk_renderer_resize(VkRenderer*, int, int) { return 0; }
extern "C" void vk_renderer_destroy(VkRenderer* renderer) {
    if (renderer) av_buffer_unref(&renderer->hardwareDevice);
}
