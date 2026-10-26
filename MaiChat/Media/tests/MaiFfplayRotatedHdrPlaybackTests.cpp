// Exercise the real hosted FFplay path with a generated, rotated HLG HEVC MOV.
// The generator and build command live in run_rotated_hdr_playback_test.py.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../ios/MaiChat/MaiChat/Media/MaiFfplayIosBridge.h"
#include "../FfplayCompat/MaiFfplayEntry.h"

namespace {

std::atomic<unsigned> presentedFrames{0};
std::atomic<unsigned> hdrFrames{0};
std::atomic<unsigned> nativeHdrFrames{0};
std::atomic<unsigned> nonFlatFrames{0};
std::atomic<unsigned> filterLinkErrors{0};

void onLog(void*, int, const char* line) {
    if (line && std::strstr(line, "Impossible to convert between the formats"))
        ++filterLinkErrors;
}

bool onFrame(const MaiVideoFrame* frame) {
    if (!frame || frame->width < 1 || frame->height < 1 || !frame->data[0])
        return false;
    ++presentedFrames;
    if (frame->color_transfer == MAI_VIDEO_TRANSFER_HLG) ++hdrFrames;
    if (frame->format == MAI_VIDEO_PIXEL_P010 &&
        frame->color_space == MAI_VIDEO_COLOR_BT2020 &&
        frame->color_transfer == MAI_VIDEO_TRANSFER_HLG)
        ++nativeHdrFrames;
    if (frame->width >= 16 && frame->height >= 8 && frame->linesize[0] != 0) {
        const bool tenBit = frame->format == MAI_VIDEO_PIXEL_P010 ||
                            frame->format == MAI_VIDEO_PIXEL_I010;
        unsigned lowest = UINT32_MAX;
        unsigned highest = 0;
        for (unsigned y = 0; y < 8; ++y) {
            const auto* row = frame->data[0] +
                              (frame->height * y / 8) * frame->linesize[0];
            for (unsigned x = 0; x < 16; ++x) {
                const unsigned column = frame->width * x / 16;
                unsigned value = row[column];
                if (tenBit) {
                    uint16_t sample = 0;
                    std::memcpy(&sample, row + column * 2, sizeof(sample));
                    value = sample;
                }
                if (value < lowest) lowest = value;
                if (value > highest) highest = value;
            }
        }
        if (highest > lowest + (tenBit ? 200 : 3)) ++nonFlatFrames;
    }
    return true;
}

}  // namespace

extern "C" bool maiGraphicsPresenterShowVideoFrameWithSubtitle(
    uint64_t, const MaiVideoFrame* frame, const MaiVideoSubtitle*, bool) {
    return onFrame(frame);
}

extern "C" bool maiGraphicsPresenterShowFrame(
    uint64_t, const uint8_t* pixels, uint32_t width, uint32_t height,
    uint32_t stride, bool) {
    if (!pixels || !width || !height || stride < width * 4) return false;
    ++presentedFrames;
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3 || (std::strcmp(argv[2], "rotated-hdr") != 0 &&
                      std::strcmp(argv[2], "straight-hdr") != 0 &&
                      std::strcmp(argv[2], "sdr") != 0)) {
        std::fprintf(stderr, "usage: %s movie.mov {rotated-hdr|straight-hdr|sdr}\n",
                     argv[0]);
        return 2;
    }
    const bool isHdr = std::strcmp(argv[2], "sdr") != 0;
    if (maiFfplaySourceHasHdrWithoutSubtitles(argv[1]) != (isHdr ? 1 : 0)) {
        std::fprintf(stderr, "fixture selected the wrong iOS decoder path\n");
        return 3;
    }
    maiFfplaySetDiagnosticSink(onLog, nullptr);
    MaiFfplayIosSession* session = maiFfplayIosStart(1, argv[1]);
    if (!session) {
        maiFfplaySetDiagnosticSink(nullptr, nullptr);
        std::fprintf(stderr, "failed to start rotated HDR playback\n");
        return 4;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (nonFlatFrames == 0 && maiFfplayIosHasFinished(session) != 1 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const unsigned frames = presentedFrames;
    const unsigned hdr = hdrFrames;
    const unsigned nativeHdr = nativeHdrFrames;
    const unsigned nonFlat = nonFlatFrames;
    const unsigned errors = filterLinkErrors;
    const bool finishedBeforeStop = maiFfplayIosHasFinished(session) == 1;
    const int exitCode = finishedBeforeStop ? maiFfplayIosExitCode(session) : 0;
    maiFfplayIosStop(session);
    maiFfplaySetDiagnosticSink(nullptr, nullptr);
    std::printf("scenario=%s presented=%u non_flat=%u hdr=%u p010_bt2020_hlg=%u filter_link_errors=%u finished_before_stop=%d exit=%d\n",
                argv[2], frames, nonFlat, hdr, nativeHdr, errors,
                finishedBeforeStop ? 1 : 0, exitCode);
    if (nonFlat == 0) {
        std::fprintf(stderr, "movie produced no non-flat video frame\n");
        return 5;
    }
    if (errors != 0) {
        std::fprintf(stderr, "hardware filter link failed before a frame\n");
        return 6;
    }
    if (isHdr && nativeHdr == 0) {
        std::fprintf(stderr, "HDR movie lost native P010/BT.2020/HLG output\n");
        return 7;
    }
    return 0;
}
