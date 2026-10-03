#include "MaiFfplayEntry.h"

#include <cerrno>

extern "C" {
#include "ffplay_renderer.h"
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

// FFplay's alternate renderer receives the AVFrame before the SDL texture
// conversion. The hosted implementation sends that frame to Graphics; it does
// not create a Vulkan device.
struct VkRenderer {
    MaiFfplayHost host{};
    AVBufferRef* hardwareDevice = nullptr;
};

extern "C" MAI_FFPLAY_EXPORT int maiFfplaySourceHasHdrWithoutSubtitles(const char* path) {
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
                 codec->color_trc == AVCOL_TRC_SMPTE2084))
                hdr = true;
        }
    }
    avformat_close_input(&input);
    return hdr && !subtitles ? 1 : 0;
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
